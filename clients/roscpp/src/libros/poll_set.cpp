/*
 * Software License Agreement (BSD License)
 *
 *  Copyright (c) 2008, Willow Garage, Inc.
 *  All rights reserved.
 *
 *  Redistribution and use in source and binary forms, with or without
 *  modification, are permitted provided that the following conditions
 *  are met:
 *
 *   * Redistributions of source code must retain the above copyright
 *     notice, this list of conditions and the following disclaimer.
 *   * Redistributions in binary form must reproduce the above
 *     copyright notice, this list of conditions and the following
 *     disclaimer in the documentation and/or other materials provided
 *     with the distribution.
 *   * Neither the name of Willow Garage, Inc. nor the names of its
 *     contributors may be used to endorse or promote products derived
 *     from this software without specific prior written permission.
 *
 *  THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 *  "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 *  LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS
 *  FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE
 *  COPYRIGHT OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT,
 *  INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING,
 *  BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
 *  LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER
 *  CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 *  LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN
 *  ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 *  POSSIBILITY OF SUCH DAMAGE.
 */

#include "ros/poll_set.h"
#include "ros/file_log.h"

#include "ros/transport/transport.h"

#include <ros/assert.h>

#include <boost/bind.hpp>

#include <fcntl.h>

namespace ros
{


PollSet::PollSet()
: sockets_changed_(false)
{
  if ( create_signal_pair(signal_pipe_) != 0 ) {
    ROS_FATAL("create_signal_pair() failed");
    ROS_BREAK();
  }
  addSocket(signal_pipe_[0], boost::bind(&PollSet::onLocalPipeEvents, this, _1));
  addEvents(signal_pipe_[0], POLLIN);
}

PollSet::~PollSet()
{
  close_signal_pair(signal_pipe_);
}

bool PollSet::addSocket(int fd, const SocketUpdateFunc& update_func, const TransportPtr& transport)
{
  SocketInfo info;
  info.fd_ = fd;
  info.events_ = 0;
  info.transport_ = transport;
  info.func_ = update_func;


  /** TCP or UDP ??? */
  int opt_domain = 0;
  int opt_type = 0;
  socklen_t optlen = sizeof(int);

  // see if the SO_BROADCAST flag is set:
  if (getsockopt(fd, SOL_SOCKET, SO_DOMAIN, &opt_domain, &optlen))
  {
    ROS_ERROR("getsockopt returned an error: errno %d %s", errno, strerror(errno));
  }
  if (getsockopt(fd, SOL_SOCKET, SO_TYPE, &opt_type, &optlen))
  {
    ROS_ERROR("getsockopt returned an error: errno %d %s", errno, strerror(errno));
  }

  if (opt_domain == AF_INET)
  {
    if (opt_type == SOCK_STREAM)
    {
      info.tcp_socket_ = AsyncTcpSocketPtr(new boost::asio::ip::tcp::socket(io_service_));
      info.tcp_socket_->assign(boost::asio::ip::tcp::v4(), fd);
    }
    else if (opt_type == SOCK_DGRAM)
    {
      info.udp_socket_ = AsyncUdpSocketPtr(new boost::asio::ip::udp::socket(io_service_));
      info.udp_socket_->assign(boost::asio::ip::udp::v4(), fd);
    }
  }
  else if (opt_domain == AF_INET6)
  {
    if (opt_type == SOCK_STREAM)
    {
      info.tcp_socket_ = AsyncTcpSocketPtr(new boost::asio::ip::tcp::socket(io_service_));
      info.tcp_socket_->assign(boost::asio::ip::tcp::v6(), fd);
    }
    else if (opt_type == SOCK_DGRAM)
    {
      info.udp_socket_ = AsyncUdpSocketPtr(new boost::asio::ip::udp::socket(io_service_));
      info.udp_socket_->assign(boost::asio::ip::udp::v6(), fd);
    }
  }

  if (!info.tcp_socket_ && !info.udp_socket_)
  {
    ROS_WARN("Socket family / type combination not supported %d / %d", opt_domain, opt_type);
  }
  /** */


  {
    boost::mutex::scoped_lock lock(socket_info_mutex_);

    bool b = socket_info_.insert(std::make_pair(fd, info)).second;
    if (!b)
    {
      ROSCPP_LOG_DEBUG("PollSet: Tried to add duplicate fd [%d]", fd);
      return false;
    }

    sockets_changed_ = true;
  }

  signal();

  return true;
}

bool PollSet::delSocket(int fd)
{
  if(fd < 0)
  {
    return false;
  }

  boost::mutex::scoped_lock lock(socket_info_mutex_);
  M_SocketInfo::iterator it = socket_info_.find(fd);
  if (it != socket_info_.end())
  {
    socket_info_.erase(it);

    {
      boost::mutex::scoped_lock lock(just_deleted_mutex_);
      just_deleted_.push_back(fd);
    }

    sockets_changed_ = true;
    signal();

    return true;
  }

  ROSCPP_LOG_DEBUG("PollSet: Tried to delete fd [%d] which is not being tracked", fd);

  return false;
}


bool PollSet::addEvents(int sock, int events)
{
  boost::mutex::scoped_lock lock(socket_info_mutex_);

  M_SocketInfo::iterator it = socket_info_.find(sock);

  if (it == socket_info_.end())
  {
    ROSCPP_LOG_DEBUG("PollSet: Tried to add events [%d] to fd [%d] which does not exist in this pollset", events, sock);
    return false;
  }

  it->second.events_ |= events;

  startOperations(it->second, events);

  signal();

  return true;
}

bool PollSet::delEvents(int sock, int events)
{
  boost::mutex::scoped_lock lock(socket_info_mutex_);

  M_SocketInfo::iterator it = socket_info_.find(sock);
  if (it != socket_info_.end())
  {
    it->second.events_ &= ~events;
  }
  else
  {
    ROSCPP_LOG_DEBUG("PollSet: Tried to delete events [%d] to fd [%d] which does not exist in this pollset", events, sock);
    return false;
  }

  startOperations(it->second, events);

  signal();

  return true;
}

void PollSet::signal()
{
  boost::mutex::scoped_try_lock lock(signal_mutex_);

  if (lock.owns_lock())
  {
    char b = 0;
    if (write_signal(signal_pipe_[1], &b, 1) < 0)
    {
      // do nothing... this prevents warnings on gcc 4.3
    }
  }
}

void PollSet::update(int poll_timeout)
{
  boost::asio::deadline_timer t(io_service_, boost::posix_time::milliseconds(poll_timeout));
  t.async_wait(boost::bind(&boost::asio::io_service::stop, &io_service_));

  io_service_.reset();
  io_service_.run();

#if 0
  createNativePollset();

  // Poll across the sockets we're servicing
  int ret;
  size_t ufds_count = ufds_.size();
  if((ret = poll_sockets(&ufds_.front(), ufds_count, poll_timeout)) < 0)
  {
	  ROS_ERROR_STREAM("poll failed with error " << last_socket_error_string());
    }
  else if (ret > 0)  // ret = 0 implies the poll timed out, nothing to do
  {
    // We have one or more sockets to service
    for(size_t i=0; i<ufds_count; i++)
    {
      if (ufds_[i].revents == 0)
      {
        continue;
      }

      SocketUpdateFunc func;
      TransportPtr transport;
      int events = 0;
      {
        boost::mutex::scoped_lock lock(socket_info_mutex_);
        M_SocketInfo::iterator it = socket_info_.find(ufds_[i].fd);
        // the socket has been entirely deleted
        if (it == socket_info_.end())
        {
          continue;
        }

        const SocketInfo& info = it->second;

        // Store off the function and transport in case the socket is deleted from another thread
        func = info.func_;
        transport = info.transport_;
        events = info.events_;
      }

      // If these are registered events for this socket, OR the events are ERR/HUP/NVAL,
      // call through to the registered function
      int revents = ufds_[i].revents;
      if (func
          && ((events & revents)
              || (revents & POLLERR)
              || (revents & POLLHUP)
              || (revents & POLLNVAL)))
      {
        bool skip = false;
        if (revents & (POLLNVAL|POLLERR|POLLHUP))
        {
          // If a socket was just closed and then the file descriptor immediately reused, we can
          // get in here with what we think is a valid socket (since it was just re-added to our set)
          // but which is actually referring to the previous fd with the same #.  If this is the case,
          // we ignore the first instance of one of these errors.  If it's a real error we'll
          // hit it again next time through.
          boost::mutex::scoped_lock lock(just_deleted_mutex_);
          if (std::find(just_deleted_.begin(), just_deleted_.end(), ufds_[i].fd) != just_deleted_.end())
          {
            skip = true;
          }
        }

        if (!skip)
        {
          func(revents & (events|POLLERR|POLLHUP|POLLNVAL));
        }
      }

      ufds_[i].revents = 0;
    }

    boost::mutex::scoped_lock lock(just_deleted_mutex_);
    just_deleted_.clear();
  }
#endif
}

void PollSet::createNativePollset()
{
  boost::mutex::scoped_lock lock(socket_info_mutex_);

  if (!sockets_changed_)
  {
    return;
  }

  // Build the list of structures to pass to poll for the sockets we're servicing
  ufds_.resize(socket_info_.size());
  M_SocketInfo::iterator sock_it = socket_info_.begin();
  M_SocketInfo::iterator sock_end = socket_info_.end();
  for (int i = 0; sock_it != sock_end; ++sock_it, ++i)
  {
    const SocketInfo& info = sock_it->second;
    socket_pollfd& pfd = ufds_[i];
    pfd.fd = info.fd_;
    pfd.events = info.events_;
    pfd.revents = 0;
  }
}

void PollSet::onLocalPipeEvents(int events)
{
  if(events & POLLIN)
  {
    char b;
    while(read_signal(signal_pipe_[0], &b, 1) > 0)
    {
      //do nothing keep draining
    };
  }

}

void PollSet::handleRead(boost::system::error_code ec, int fd)
{
  SocketUpdateFunc func;
  TransportPtr transport;
  int events = 0;

  {
    boost::mutex::scoped_lock lock(socket_info_mutex_);
    M_SocketInfo::iterator it = socket_info_.find(fd);
    // the socket has been entirely deleted
    if (it == socket_info_.end())
    {
      return;
    }

    SocketInfo& info = it->second;
    func = info.func_;
    transport = info.transport_;
    events = info.events_;
  }

  if (!ec && func) {
    func(POLLIN);
  } else {
    ROS_WARN("GA GA: %d %04X %d", __LINE__, events, ec );
  }

  if (!ec || ec == boost::asio::error::would_block)
  {
    boost::mutex::scoped_lock lock(socket_info_mutex_);
    M_SocketInfo::iterator it = socket_info_.find(fd);
    // the socket has been entirely deleted
    if (it == socket_info_.end())
    {
      return;
    }

    SocketInfo& info = it->second;
    startOperations(info, POLLIN);
  }
}

void PollSet::handleWrite(boost::system::error_code ec, int fd)
{
  SocketUpdateFunc func;
  TransportPtr transport;
  int events = 0;

  {
    boost::mutex::scoped_lock lock(socket_info_mutex_);
    M_SocketInfo::iterator it = socket_info_.find(fd);
    // the socket has been entirely deleted
    if (it == socket_info_.end())
    {
      return;
    }

    SocketInfo& info = it->second;
    func = info.func_;
    transport = info.transport_;
    events = info.events_;
  }

  if (!ec && func) {
    func(POLLOUT);
  } else {
    ROS_WARN("GA GA: %d %04X %d", __LINE__, events, ec );
  }

  if (!ec || ec == boost::asio::error::would_block)
  {
    boost::mutex::scoped_lock lock(socket_info_mutex_);
    M_SocketInfo::iterator it = socket_info_.find(fd);
    // the socket has been entirely deleted
    if (it == socket_info_.end())
    {
      return;
    }

    SocketInfo& info = it->second;
    startOperations(info, POLLOUT);
  }
}

void PollSet::startOperations(SocketInfo& info, int events)
{
  if ((info.events_ & events) & (POLLIN | POLLPRI))
  {
    if (info.tcp_socket_)
    {
      info.tcp_socket_->async_read_some(
          boost::asio::null_buffers(),
          boost::bind(&PollSet::handleRead,
                      this,
                      boost::asio::placeholders::error,
                      info.fd_));
    }
    if (info.udp_socket_)
    {
      info.udp_socket_->async_receive(
          boost::asio::null_buffers(),
          boost::bind(&PollSet::handleRead,
                      this,
                      boost::asio::placeholders::error,
                      info.fd_));
    }
  }

  if ((info.events_ & events) & POLLOUT)
  {
    if (info.tcp_socket_)
    {
      info.tcp_socket_->async_write_some(
          boost::asio::null_buffers(),
          boost::bind(&PollSet::handleWrite,
                      this,
                      boost::asio::placeholders::error,
                      info.fd_));
    }
    if (info.udp_socket_)
    {
      info.udp_socket_->async_send(
          boost::asio::null_buffers(),
          boost::bind(&PollSet::handleWrite,
                      this,
                      boost::asio::placeholders::error,
                      info.fd_));

    }
  }
}


}

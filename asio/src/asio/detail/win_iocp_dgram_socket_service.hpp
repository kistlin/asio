//
// win_iocp_dgram_socket_service.hpp
// ~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
//
// Copyright (c) 2003 Christopher M. Kohlhoff (chris@kohlhoff.com)
//
// Permission to use, copy, modify, distribute and sell this software and its
// documentation for any purpose is hereby granted without fee, provided that
// the above copyright notice appears in all copies and that both the copyright
// notice and this permission notice appear in supporting documentation. This
// software is provided "as is" without express or implied warranty, and with
// no claim as to its suitability for any purpose.
//

#ifndef ASIO_DETAIL_REACTIVE_DGRAM_SOCKET_SERVICE_HPP
#define ASIO_DETAIL_REACTIVE_DGRAM_SOCKET_SERVICE_HPP

#include "asio/detail/push_options.hpp"

#include "asio/detail/push_options.hpp"
#include <boost/bind.hpp>
#include "asio/detail/pop_options.hpp"

#include "asio/basic_demuxer.hpp"
#include "asio/completion_context.hpp"
#include "asio/service_factory.hpp"
#include "asio/socket_error.hpp"
#include "asio/detail/socket_holder.hpp"
#include "asio/detail/socket_ops.hpp"
#include "asio/detail/socket_types.hpp"
#include "asio/detail/win_iocp_demuxer_service.hpp"

namespace asio {
namespace detail {

class win_iocp_dgram_socket_service
{
public:
  // The native type of the dgram socket. This type is dependent on the
  // underlying implementation of the socket layer.
  typedef socket_type impl_type;

  // Constructor. This dgram_socket service can only work if the demuxer is
  // using the win_iocp_demuxer_service. By using this type as the parameter we
  // will cause a compile error if this is not the case.
  win_iocp_dgram_socket_service(
      basic_demuxer<win_iocp_demuxer_service>& demuxer)
    : demuxer_service_(demuxer.get_service(
          service_factory<win_iocp_demuxer_service>()))
  {
  }

  // Return a null dgram socket implementation.
  static impl_type null()
  {
    return invalid_socket;
  }

  // Create a new dgram socket implementation.
  template <typename Address>
  void create(impl_type& impl, const Address& address)
  {
    socket_holder sock(socket_ops::socket(address.family(), SOCK_DGRAM,
          IPPROTO_UDP));
    if (sock.get() == invalid_socket)
      throw socket_error(socket_ops::get_error());

    int reuse = 1;
    socket_ops::setsockopt(sock.get(), SOL_SOCKET, SO_REUSEADDR, &reuse,
        sizeof(reuse));

    if (socket_ops::bind(sock.get(), address.native_address(),
          address.native_size()) == socket_error_retval)
      throw socket_error(socket_ops::get_error());

    demuxer_service_.register_socket(sock.get());

    impl = sock.release();
  }

  // Destroy a dgram socket implementation.
  void destroy(impl_type& impl)
  {
    if (impl != null())
    {
      socket_ops::close(impl);
      impl = null();
    }
  }

  // Send a datagram to the specified address. Returns the number of bytes
  // sent. Throws a socket_error exception on failure.
  template <typename Address>
  size_t sendto(impl_type& impl, const void* data, size_t length,
      const Address& destination)
  {
    int bytes_sent = socket_ops::sendto(impl, data, length, 0,
        destination.native_address(), destination.native_size());
    if (bytes_sent < 0)
      throw socket_error(socket_ops::get_error());
    return bytes_sent;
  }

  template <typename Handler>
  class sendto_operation
    : public win_iocp_demuxer_service::operation
  {
  public:
    sendto_operation(completion_context& context, bool context_acquired,
        Handler handler)
      : win_iocp_demuxer_service::operation(context, context_acquired),
        handler_(handler)
    {
    }

    virtual void do_completion(DWORD last_error, size_t bytes_transferred)
    {
      socket_error error(last_error);
      do_upcall(handler_, error, bytes_transferred);
      delete this;
    }

    static void do_upcall(Handler handler, const socket_error& error,
        size_t bytes_sent)
    {
      try
      {
        handler(error, bytes_sent);
      }
      catch (...)
      {
      }
    }

  private:
    Handler handler_;
  };

  // Start an asynchronous send. The data being sent must be valid for the
  // lifetime of the asynchronous operation.
  template <typename Address, typename Handler>
  void async_sendto(impl_type& impl, const void* data, size_t length,
      const Address& destination, Handler handler,
      completion_context& context)
  {
    sendto_operation<Handler>* sendto_op =
      new sendto_operation<Handler>(context, false, handler);

    demuxer_service_.operation_started();

    WSABUF buf;
    buf.len = length;
    buf.buf = static_cast<char*>(const_cast<void*>(data));
    DWORD bytes_transferred = 0;

    int result = ::WSASendTo(impl, &buf, 1, &bytes_transferred, 0,
        destination.native_address(), destination.native_size(), sendto_op, 0);
    DWORD last_error = ::WSAGetLastError();

    if (result != 0 && last_error != WSA_IO_PENDING)
    {
      delete sendto_op;
      socket_error error(last_error);
      demuxer_service_.operation_completed(
          boost::bind(handler, error, bytes_transferred), context, false);
    }
  }

  // Receive a datagram with the address of the sender. Returns the number of
  // bytes received. Throws a socket_error exception on failure.
  template <typename Address>
  size_t recvfrom(impl_type& impl, void* data, size_t max_length,
      Address& sender_address)
  {
    socket_addr_len_type addr_len = sender_address.native_size();
    int bytes_recvd = socket_ops::recvfrom(impl, data, max_length, 0,
        sender_address.native_address(), &addr_len);
    if (bytes_recvd < 0)
      throw socket_error(socket_ops::get_error());
    sender_address.native_size(addr_len);
    return bytes_recvd;
  }

  template <typename Address, typename Handler>
  class recvfrom_operation
    : public win_iocp_demuxer_service::operation
  {
  public:
    recvfrom_operation(completion_context& context, bool context_acquired,
        Address& address, Handler handler)
      : win_iocp_demuxer_service::operation(context, context_acquired),
        address_(address),
        address_size_(address.native_size()),
        handler_(handler)
    {
    }

    int& address_size()
    {
      return address_size_;
    }

    virtual void do_completion(DWORD last_error, size_t bytes_transferred)
    {
      address_.native_size(address_size_);
      socket_error error(last_error);
      do_upcall(handler_, error, bytes_transferred);
      delete this;
    }

    static void do_upcall(const Handler& handler, const socket_error& error,
        size_t bytes_recvd)
    {
      try
      {
        handler(error, bytes_recvd);
      }
      catch (...)
      {
      }
    }

  private:
    Address& address_;
    int address_size_;
    Handler handler_;
  };

  // Start an asynchronous receive. The buffer for the data being received and
  // the sender_address obejct must both be valid for the lifetime of the
  // asynchronous operation.
  template <typename Address, typename Handler>
  void async_recvfrom(impl_type& impl, void* data, size_t max_length,
      Address& sender_address, Handler handler, completion_context& context)
  {
    recvfrom_operation<Address, Handler>* recvfrom_op =
      new recvfrom_operation<Address, Handler>(context, false, sender_address,
          handler);

    demuxer_service_.operation_started();

    WSABUF buf;
    buf.len = max_length;
    buf.buf = static_cast<char*>(data);
    DWORD bytes_transferred = 0;
    DWORD flags = 0;

    int result = ::WSARecvFrom(impl, &buf, 1, &bytes_transferred, &flags,
        sender_address.native_address(), &recvfrom_op->address_size(),
        recvfrom_op, 0);
    DWORD last_error = ::WSAGetLastError();

    if (result != 0 && last_error != WSA_IO_PENDING)
    {
      delete recvfrom_op;
      socket_error error(last_error);
      demuxer_service_.operation_completed(
          boost::bind(handler, error, bytes_transferred), context, false);
    }
  }

private:
  // The demuxer service used for running asynchronous operations.
  win_iocp_demuxer_service& demuxer_service_;
};

} // namespace detail
} // namespace asio

#include "asio/detail/pop_options.hpp"

#endif // ASIO_DETAIL_REACTIVE_DGRAM_SOCKET_SERVICE_HPP

#include <Poco/Net/NetException.h>

#include <IO/ReadBufferFromPocoSocket.h>
#include <IO/TimeoutSetter.h>
#include <Common/Exception.h>
#include <Common/NetException.h>
#include <Common/Stopwatch.h>


namespace ProfileEvents
{
    extern const Event NetworkReceiveElapsedMicroseconds;
}


namespace DB
{
namespace ErrorCodes
{
    extern const int NETWORK_ERROR;
    extern const int SOCKET_TIMEOUT;
    extern const int CANNOT_READ_FROM_SOCKET;
}


bool ReadBufferFromPocoSocket::nextImpl()
{
    ssize_t bytes_read = 0;
    Stopwatch watch;

    /// Add more details to exceptions.
    try
    {
        /// If async_callback is specified, and read will block, run async_callback and try again later.
        /// It is expected that file descriptor may be polled externally.
        /// Note that receive timeout is not checked here. External code should check it while polling.
        while (async_callback && !socket.poll(0, Poco::Net::Socket::SELECT_READ))
            async_callback(socket.impl()->sockfd(), socket.getReceiveTimeout(), socket_description);

        /// receiveBytes in SecureStreamSocket throws TimeoutException after max(receive_timeout, send_timeout),
        /// but we want to get this exception exactly after receive_timeout. So, set send_timeout = receive_timeout
        /// before receiveBytes.
        std::unique_ptr<TimeoutSetter> timeout_setter = nullptr;
        if (socket.secure())
            timeout_setter = std::make_unique<TimeoutSetter>(dynamic_cast<Poco::Net::StreamSocket &>(socket), socket.getReceiveTimeout(), socket.getReceiveTimeout());

        bytes_read = socket.impl()->receiveBytes(internal_buffer.begin(), internal_buffer.size());
    }
    catch (const Poco::Net::NetException & e)
    {
        throw NetException(e.displayText() + ", while reading from socket (" + peer_address.toString() + ")", ErrorCodes::NETWORK_ERROR);
    }
    catch (const Poco::TimeoutException &)
    {
        throw NetException("Timeout exceeded while reading from socket (" + peer_address.toString() + ")", ErrorCodes::SOCKET_TIMEOUT);
    }
    catch (const Poco::IOException & e)
    {
        throw NetException(e.displayText() + ", while reading from socket (" + peer_address.toString() + ")", ErrorCodes::NETWORK_ERROR);
    }

    if (bytes_read < 0)
        throw NetException("Cannot read from socket (" + peer_address.toString() + ")", ErrorCodes::CANNOT_READ_FROM_SOCKET);

    /// NOTE: it is quite inaccurate on high loads since the thread could be replaced by another one
    ProfileEvents::increment(ProfileEvents::NetworkReceiveElapsedMicroseconds, watch.elapsedMicroseconds());

    if (bytes_read)
        working_buffer.resize(bytes_read);
    else
        return false;

    return true;
}

ReadBufferFromPocoSocket::ReadBufferFromPocoSocket(Poco::Net::Socket & socket_, size_t buf_size)
    : BufferWithOwnMemory<ReadBuffer>(buf_size)
    , socket(socket_)
    , peer_address(socket.peerAddress())
    , socket_description("socket (" + peer_address.toString() + ")")
{
}

bool ReadBufferFromPocoSocket::poll(size_t timeout_microseconds) const
{
    return available() || socket.poll(timeout_microseconds, Poco::Net::Socket::SELECT_READ | Poco::Net::Socket::SELECT_ERROR);
}

}

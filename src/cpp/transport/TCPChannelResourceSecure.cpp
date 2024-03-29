// Copyright 2018 Proyectos y Sistemas de Mantenimiento SL (eProsima).
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include <fastrtps/transport/TCPChannelResourceSecure.h>
#include <fastrtps/transport/TCPTransportInterface.h>
#include <fastrtps/utils/IPLocator.h>
#include <fastrtps/utils/eClock.h>

#include <future>

namespace eprosima {
namespace fastrtps {
namespace rtps {

using namespace asio;

TCPChannelResourceSecure::TCPChannelResourceSecure(
        TCPTransportInterface* parent,
        asio::io_service& service,
        asio::ssl::context& ssl_context,
        const Locator_t& locator,
        uint32_t maxMsgSize)
    : TCPChannelResource(parent, locator, maxMsgSize)
    , service_(service)
    , ssl_context_(ssl_context)
    , strand_(service)
{
}

TCPChannelResourceSecure::TCPChannelResourceSecure(
        TCPTransportInterface* parent,
        asio::io_service& service,
        asio::ssl::context& ssl_context,
        std::shared_ptr<asio::ssl::stream<asio::ip::tcp::socket>> socket,
        uint32_t maxMsgSize)
    : TCPChannelResource(parent, maxMsgSize)
    , service_(service)
    , ssl_context_(ssl_context)
    , strand_(service)
    , secure_socket_(socket)
{
    set_tls_verify_mode(parent->configuration());
}

TCPChannelResourceSecure::~TCPChannelResourceSecure()
{
    disconnect();
}

void TCPChannelResourceSecure::connect(
        const std::shared_ptr<TCPChannelResource>& myself)
{
    assert(TCPConnectionType::TCP_CONNECT_TYPE == tcp_connection_type_);
    using asio::ip::tcp;
    using TLSHSRole = TCPTransportDescriptor::TLSConfig::TLSHandShakeRole;
    eConnectionStatus expected = eConnectionStatus::eDisconnected;

    if (connection_status_.compare_exchange_strong(expected, eConnectionStatus::eConnecting))
    {
        try
        {
            ip::tcp::resolver resolver(service_);

            auto endpoints = resolver.resolve(
                IPLocator::hasWan(locator_) ? IPLocator::toWanstring(locator_) : IPLocator::ip_to_string(locator_),
                std::to_string(IPLocator::getPhysicalPort(locator_)));

            TCPTransportInterface* parent = parent_;
            secure_socket_ = std::make_shared<asio::ssl::stream<asio::ip::tcp::socket>>(service_, ssl_context_);
            set_tls_verify_mode(parent->configuration());
            const auto secure_socket = secure_socket_;
            const auto channel = myself;

            asio::async_connect(secure_socket_->lowest_layer(), endpoints,
                [secure_socket, channel, parent](const std::error_code& error,
                    const tcp::endpoint& /*endpoint*/)
            {
                if (!error)
                {
                    ssl::stream_base::handshake_type role = ssl::stream_base::client;
                    if (parent->configuration()->tls_config.handshake_role == TLSHSRole::SERVER)
                    {
                        role = ssl::stream_base::server;
                    }

                    logInfo(RTCP_TLS, "Connected: " << IPLocator::to_string(channel->locator()));

                    secure_socket->async_handshake(role,
                        [channel, parent](const std::error_code& error)
                    {
                        if (!error)
                        {
                            logInfo(RTCP_TLS, "Handshake OK: " << IPLocator::to_string(channel->locator()));
                            parent->SocketConnected(channel, error);
                        }
                        else
                        {
                            logError(RTCP_TLS, "Handshake failed: " << error.message());
                            eClock::my_sleep(5000); // Retry, but after a big while
                            parent->SocketConnected(channel, error);
                        }
                    });
                }
                else
                {
                    //logError(RTCP_TLS, "Connect failed: " << error.message());
                    parent->SocketConnected(channel, error); // Manages errors and retries
                }
            });
        }
        catch(const std::system_error &error)
        {
            logError(RTCP, "Openning socket " << error.what());
        }
    }
}

void TCPChannelResourceSecure::disconnect()
{
    if (eConnecting < change_status(eConnectionStatus::eDisconnected))
    {
        std::promise<bool> cancel_promise;
        auto cancel_future = cancel_promise.get_future();
        auto socket = secure_socket_;

        secure_socket_->async_shutdown([&, socket](const std::error_code&)
                {
                    cancel_promise.set_value(true);
                });

        cancel_future.get();
    }
}

uint32_t TCPChannelResourceSecure::read(
        octet* buffer,
        const std::size_t size,
        asio::error_code& ec)
{
    size_t bytes_read = 0;
    std::promise<size_t> bytes_promise;
    auto bytes_future = bytes_promise.get_future();
    auto socket = secure_socket_;

    strand_.post([&, socket]()
            {
                asio::async_read(*socket, asio::buffer(buffer, size),
                        [&, socket](const std::error_code& error, const size_t bytes_transferred)
                        {
                            ec = error;

                            if (!error)
                            {
                                bytes_promise.set_value(bytes_transferred);
                            }
                            else
                            {
                                bytes_promise.set_value(0);
                            }
                        });
            });
    bytes_read = bytes_future.get();

    return static_cast<uint32_t>(bytes_read);
}

size_t TCPChannelResourceSecure::send(
        const octet* header,
        size_t header_size,
        const octet* data,
        size_t size,
        asio::error_code& ec,
        bool blocking)
{
    size_t bytes_sent = 0;

    if (eConnecting < connection_status_)
    {
        std::vector<asio::const_buffer> buffers;
        if(header_size > 0)
        {
            buffers.push_back(asio::buffer(header, header_size));
        }
        buffers.push_back(asio::buffer(data, size));

        if(blocking)
        {
            std::promise<size_t> bytes_promise;
            auto bytes_future = bytes_promise.get_future();
            auto socket = secure_socket_;

            strand_.post([&, socket]()
                    {
                        asio::async_write(*socket, buffers,
                                [&, socket](const std::error_code& error, const size_t& bytes_transferred)
                                {
                                    ec = error;

                                    if (!error)
                                    {
                                        bytes_promise.set_value(bytes_transferred);
                                    }
                                    else
                                    {
                                        bytes_promise.set_value(0);
                                    }
                                });
                    });
            bytes_sent = bytes_future.get();
        }
        else
        {
            bytes_sent = secure_socket_->write_some(buffers, ec);
        }
    }

    return bytes_sent;
}

asio::ip::tcp::endpoint TCPChannelResourceSecure::remote_endpoint() const
{
    return secure_socket_->lowest_layer().remote_endpoint();
}

asio::ip::tcp::endpoint TCPChannelResourceSecure::local_endpoint() const
{
    return secure_socket_->lowest_layer().local_endpoint();
}

void TCPChannelResourceSecure::set_options(const TCPTransportDescriptor* options)
{
    secure_socket_->lowest_layer().set_option(socket_base::receive_buffer_size(options->receiveBufferSize));
    secure_socket_->lowest_layer().set_option(socket_base::send_buffer_size(options->sendBufferSize));
    secure_socket_->lowest_layer().set_option(ip::tcp::no_delay(options->enable_tcp_nodelay));
}

void TCPChannelResourceSecure::set_tls_verify_mode(const TCPTransportDescriptor* options)
{
    using TLSVerifyMode = TCPTransportDescriptor::TLSConfig::TLSVerifyMode;

    if (options->apply_security)
    {
        if (options->tls_config.verify_mode != TLSVerifyMode::UNUSED)
        {
            ssl::verify_mode vm = 0x00;
            if (options->tls_config.get_verify_mode(TLSVerifyMode::VERIFY_NONE))
            {
                vm |= ssl::verify_none;
            }
            else if (options->tls_config.get_verify_mode(TLSVerifyMode::VERIFY_PEER))
            {
                vm |= ssl::verify_peer;
            }
            else if (options->tls_config.get_verify_mode(TLSVerifyMode::VERIFY_FAIL_IF_NO_PEER_CERT))
            {
                vm |= ssl::verify_fail_if_no_peer_cert;
            }
            else if (options->tls_config.get_verify_mode(TLSVerifyMode::VERIFY_CLIENT_ONCE))
            {
                vm |= ssl::verify_client_once;
            }
            secure_socket_->set_verify_mode(vm);
        }
    }
}

void TCPChannelResourceSecure::cancel()
{
    secure_socket_->lowest_layer().cancel();
}

void TCPChannelResourceSecure::close()
{
    secure_socket_->lowest_layer().close();
}

void TCPChannelResourceSecure::shutdown(asio::socket_base::shutdown_type)
{
    secure_socket_->shutdown();
}

} // namespace rtps
} // namespace fastrtps
} // namespace eprosima

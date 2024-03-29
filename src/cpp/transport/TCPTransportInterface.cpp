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

#include <fastrtps/transport/TCPTransportInterface.h>
#include <fastrtps/transport/tcp/RTCPMessageManager.h>
#include "TCPSenderResource.hpp"
#include <fastrtps/log/Log.h>
#include <fastrtps/utils/IPLocator.h>
#include <fastrtps/utils/System.h>
#include <fastrtps/transport/TCPChannelResourceBasic.h>
#include <fastrtps/transport/TCPAcceptorBasic.h>
#if TLS_FOUND
#include <fastrtps/transport/TCPChannelResourceSecure.h>
#include <fastrtps/transport/TCPAcceptorSecure.h>
#endif
#include "timedevent/TCPKeepAliveEvent.hpp"

#include <asio/steady_timer.hpp>
#include <utility>
#include <cstring>
#include <algorithm>

using namespace std;
using namespace asio;

namespace eprosima{
namespace fastrtps{
namespace rtps {

static const int s_default_keep_alive_frequency = 10000; // 10 SECONDS
static const int s_default_keep_alive_timeout = 30000; // 30 SECONDS
//static const int s_clean_deleted_sockets_pool_timeout = 100; // 100 MILLISECONDS
static const int s_default_tcp_negotitation_timeout = 5000; // 5 Seconds

TCPTransportDescriptor::TCPTransportDescriptor()
    : SocketTransportDescriptor(s_maximumMessageSize, s_maximumInitialPeersRange)
    , keep_alive_frequency_ms(s_default_keep_alive_frequency)
    , keep_alive_timeout_ms(s_default_keep_alive_timeout)
    , max_logical_port(100)
    , logical_port_range(20)
    , logical_port_increment(2)
    , tcp_negotiation_timeout(s_default_tcp_negotitation_timeout)
    , enable_tcp_nodelay(false)
    , wait_for_tcp_negotiation(false)
    , calculate_crc(true)
    , check_crc(true)
    , apply_security(false)
{
}

TCPTransportDescriptor::TCPTransportDescriptor(const TCPTransportDescriptor& t)
    : SocketTransportDescriptor(t)
    , listening_ports(t.listening_ports)
    , keep_alive_frequency_ms(t.keep_alive_frequency_ms)
    , keep_alive_timeout_ms(t.keep_alive_timeout_ms)
    , max_logical_port(t.max_logical_port)
    , logical_port_range(t.logical_port_range)
    , logical_port_increment(t.logical_port_increment)
    , tcp_negotiation_timeout(t.tcp_negotiation_timeout)
    , enable_tcp_nodelay(t.enable_tcp_nodelay)
    , wait_for_tcp_negotiation(t.wait_for_tcp_negotiation)
    , calculate_crc(t.calculate_crc)
    , check_crc(t.check_crc)
    , apply_security(t.apply_security)
    , tls_config(t.tls_config)
{
}

TCPTransportDescriptor& TCPTransportDescriptor::operator=(const TCPTransportDescriptor& t)
{

    maxMessageSize = t.maxMessageSize;
    maxInitialPeersRange = t.maxInitialPeersRange;
    sendBufferSize = t.sendBufferSize;
    receiveBufferSize = t.receiveBufferSize;
    TTL = t.TTL;
    listening_ports = t.listening_ports;
    keep_alive_frequency_ms = t.keep_alive_frequency_ms;
    keep_alive_timeout_ms = t.keep_alive_timeout_ms;
    max_logical_port = t.max_logical_port;
    logical_port_range = t.logical_port_range;
    logical_port_increment = t.logical_port_increment;
    tcp_negotiation_timeout = t.tcp_negotiation_timeout;
    enable_tcp_nodelay = t.enable_tcp_nodelay;
    wait_for_tcp_negotiation = t.wait_for_tcp_negotiation;
    calculate_crc = t.calculate_crc;
    check_crc = t.check_crc;
    apply_security = t.apply_security;
    tls_config = t.tls_config;
    return *this;
}

#if TLS_FOUND
TCPTransportInterface::TCPTransportInterface(int32_t transport_kind)
    : TransportInterface(transport_kind)
    , ssl_context_(asio::ssl::context::sslv23)
    , keep_alive_event_(nullptr)
{
}
#else
TCPTransportInterface::TCPTransportInterface(int32_t transport_kind)
    : TransportInterface(transport_kind)
    , keep_alive_event_(nullptr)
{
    if (configuration()->apply_security)
    {
        logError(RTCP_TLS, "Trying to use TCP Transport with TLS but TLS was not found.");
    }
}
#endif

TCPTransportInterface::~TCPTransportInterface()
{
}

void TCPTransportInterface::clean()
{
    assert(receiver_resources_.size() == 0);

    if(keep_alive_event_ != nullptr)
    {
        delete keep_alive_event_;
    }

    {
        std::unique_lock<std::mutex> lock(rtcp_message_manager_mutex_);
        std::unique_lock<std::mutex> scopedLock(sockets_map_mutex_);

        for (auto& unbound_channel_resource : unbound_channel_resources_)
        {
            unbound_channel_resource->disable();
        }

        for (auto& channel_resource : channel_resources_)
        {
            if (channel_resource.second->connection_established())
            {
                rtcp_message_manager_->sendUnbindConnectionRequest(channel_resource.second);
            }

            channel_resource.second->disable();
        }
        scopedLock.unlock();

        rtcp_message_manager_cv_.wait(lock, [&]()
                {
                return 1 >=rtcp_message_manager_.use_count();
                });

        if (rtcp_message_manager_)
        {
            rtcp_message_manager_->dispose();
            rtcp_message_manager_.reset();
        }
    }

    std::unique_lock<std::mutex> scopedLock(sockets_map_mutex_);
    for (auto& unbound_channel_resource : unbound_channel_resources_)
    {
        unbound_channel_resource->thread(std::thread());
    }

    for (auto& channel_resource : channel_resources_)
    {
        channel_resource.second->thread(std::thread());
    }
    channel_resources_.clear();

    if (io_service_thread_)
    {
        io_service_.stop();
        io_service_thread_->join();
        io_service_thread_ = nullptr;
    }
}

void TCPTransportInterface::bind_socket(
        std::shared_ptr<TCPChannelResource>& channel)
{
    std::unique_lock<std::mutex> scopedLock(sockets_map_mutex_);

    auto it_remove = std::find(unbound_channel_resources_.begin(), unbound_channel_resources_.end(), channel);
    assert(it_remove != unbound_channel_resources_.end());
    unbound_channel_resources_.erase(it_remove);
    channel_resources_[channel->locator()] = channel;
}

bool TCPTransportInterface::check_crc(
        const TCPHeader &header,
        const octet *data,
        uint32_t size) const
{
    uint32_t crc(0);
    for (uint32_t i = 0; i < size; ++i)
    {
        crc = RTCPMessageManager::addToCRC(crc, data[i]);
    }
    return crc == header.crc;
}

void TCPTransportInterface::calculate_crc(
        TCPHeader &header,
        const octet *data,
        uint32_t size) const
{
    uint32_t crc(0);
    for (uint32_t i = 0; i < size; ++i)
    {
        crc = RTCPMessageManager::addToCRC(crc, data[i]);
    }
    header.crc = crc;
}


bool TCPTransportInterface::create_acceptor_socket(const Locator_t& locator)
{
    try
    {
        if (is_interface_whitelist_empty())
        {
#if TLS_FOUND
            if (configuration()->apply_security)
            {
                std::shared_ptr<TCPAcceptorSecure> acceptor =
                    std::make_shared<TCPAcceptorSecure>(io_service_, this, locator);
                acceptor->accept(this, acceptor, ssl_context_);
            }
            else
#endif
            {
                std::shared_ptr<TCPAcceptorBasic> acceptor =
                    std::make_shared<TCPAcceptorBasic>(io_service_, this, locator);
                acceptor->accept(this, acceptor);
            }

            logInfo(RTCP, " OpenAndBindInput (physical: " << IPLocator::getPhysicalPort(locator) << "; logical: "
                << IPLocator::getLogicalPort(locator) << ")");

        }
        else
        {
            std::vector<std::string> vInterfaces = get_binding_interfaces_list();
            for (std::string& sInterface : vInterfaces)
            {
#if TLS_FOUND
                if (configuration()->apply_security)
                {
                    std::shared_ptr<TCPAcceptorSecure> acceptor =
                        std::make_shared<TCPAcceptorSecure>(io_service_, sInterface, locator);
                    acceptor->accept(this, acceptor, ssl_context_);
                }
                else
#endif
                {
                    std::shared_ptr<TCPAcceptorBasic> acceptor =
                        std::make_shared<TCPAcceptorBasic>(io_service_, sInterface, locator);
                    acceptor->accept(this, acceptor);
                }

                logInfo(RTCP, " OpenAndBindInput (physical: " << IPLocator::getPhysicalPort(locator) << "; logical: "
                    << IPLocator::getLogicalPort(locator) << ")");
            }
        }
    }
    catch (asio::system_error const& e)
    {
        (void)e;
        logError(RTCP_MSG_OUT, "TCPTransport Error binding at port: (" << IPLocator::getPhysicalPort(locator) << ")" << " with msg: " << e.what());
        return false;
    }
    catch (const asio::error_code& code)
    {
        (void)code;
        logError(RTCP, "TCPTransport Error binding at port: (" << IPLocator::getPhysicalPort(locator) << ")" << " with code: " << code);
        return false;
    }

    return true;
}

void TCPTransportInterface::fill_rtcp_header(
        TCPHeader& header,
        const octet* send_buffer,
        uint32_t send_buffer_size,
        uint16_t logical_port) const
{
    header.length = send_buffer_size + static_cast<uint32_t>(TCPHeader::size());
    header.logical_port = logical_port;
    if (configuration()->calculate_crc)
    {
        calculate_crc(header, send_buffer, send_buffer_size);
    }
}

bool TCPTransportInterface::DoInputLocatorsMatch(
        const Locator_t& left,
        const Locator_t& right) const
{
    return IPLocator::getPhysicalPort(left) ==  IPLocator::getPhysicalPort(right);
}

bool TCPTransportInterface::init()
{
    apply_tls_config();
    if (configuration()->sendBufferSize == 0 || configuration()->receiveBufferSize == 0)
    {
        // Check system buffer sizes.
        ip::tcp::socket socket(io_service_);
        socket.open(generate_protocol());

        if (configuration()->sendBufferSize == 0)
        {
            socket_base::send_buffer_size option;
            socket.get_option(option);
            set_send_buffer_size(option.value());

            if (configuration()->sendBufferSize < s_minimumSocketBuffer)
            {
                set_send_buffer_size(s_minimumSocketBuffer);
            }
        }

        if (configuration()->receiveBufferSize == 0)
        {
            socket_base::receive_buffer_size option;
            socket.get_option(option);
            set_receive_buffer_size(option.value());

            if (configuration()->receiveBufferSize < s_minimumSocketBuffer)
            {
                set_receive_buffer_size(s_minimumSocketBuffer);
            }
        }

        socket.close();
    }

    if (configuration()->maxMessageSize > s_maximumMessageSize)
    {
        logError(RTCP_MSG_OUT, "maxMessageSize cannot be greater than 65000");
        return false;
    }

    if (configuration()->maxMessageSize > configuration()->sendBufferSize)
    {
        logError(RTCP_MSG_OUT, "maxMessageSize cannot be greater than send_buffer_size");
        return false;
    }

    if (configuration()->maxMessageSize > configuration()->receiveBufferSize)
    {
        logError(RTCP_MSG_OUT, "maxMessageSize cannot be greater than receive_buffer_size");
        return false;
    }

    if (!rtcp_message_manager_)
    {
        rtcp_message_manager_ = std::make_shared<RTCPMessageManager>(this);
    }

    // TODO(Ricardo) Create an event that update this list.
    get_ips(current_interfaces_);

    auto ioServiceFunction = [&]()
    {
        asio::executor_work_guard<asio::io_service::executor_type> work_(io_service_.get_executor());
        io_service_.run();
    };
    io_service_thread_ = std::make_shared<std::thread>(ioServiceFunction);

    keep_alive_event_ = new TCPKeepAliveEvent(*this, io_service_, *io_service_thread_.get(),
        configuration()->keep_alive_frequency_ms);

    if (0 < configuration()->keep_alive_frequency_ms)
    {
        keep_alive_event_->restart_timer();
    }

    return true;
}

bool TCPTransportInterface::is_input_port_open(uint16_t port) const
{
    std::unique_lock<std::mutex> scopedLock(sockets_map_mutex_);
    return receiver_resources_.find(port) != receiver_resources_.end();
}

bool TCPTransportInterface::IsInputChannelOpen(const Locator_t& locator) const
{
    return IsLocatorSupported(locator) && is_input_port_open(IPLocator::getLogicalPort(locator));
}

bool TCPTransportInterface::IsLocatorSupported(const Locator_t& locator) const
{
    return locator.kind == transport_kind_;
}

bool TCPTransportInterface::is_output_channel_open_for(const Locator_t& locator) const
{
    if (!IsLocatorSupported(locator))
    {
        return false;
    }

    std::unique_lock<std::mutex> scopedLock(sockets_map_mutex_);

    // Check if there is any socket opened with the given locator.
    auto channel_resource = channel_resources_.find(IPLocator::toPhysicalLocator(locator));

    if (channel_resource != channel_resources_.end())
    {
        // And it is registered as output logical port
        return channel_resource->second->is_logical_port_added(IPLocator::getLogicalPort(locator));
    }

    return false;
}

Locator_t TCPTransportInterface::RemoteToMainLocal(const Locator_t& remote) const
{
    if (!IsLocatorSupported(remote))
        return false;

    Locator_t mainLocal(remote);
    mainLocal.set_Invalid_Address();
    return mainLocal;
}


void TCPTransportInterface::CloseOutputChannel(std::shared_ptr<TCPChannelResource>& channel)
{
    Locator_t physical_locator = channel->locator();
    channel.reset();
    std::unique_lock<std::mutex> scopedLock(sockets_map_mutex_);
    auto channel_resource = channel_resources_.find(physical_locator);
    assert(channel_resource != channel_resources_.end());
    (void)channel_resource;
}

bool TCPTransportInterface::CloseInputChannel(const Locator_t& locator)
{
    bool bClosed = false;
    {
        std::unique_lock<std::mutex> scopedLock(sockets_map_mutex_);

        uint16_t logicalPort = IPLocator::getLogicalPort(locator);
        auto receiverIt = receiver_resources_.find(logicalPort);
        if (receiverIt != receiver_resources_.end())
        {
            bClosed = true;
            ReceiverInUseCV* receiver_in_use = receiverIt->second.second;
            receiver_resources_.erase(receiverIt);

            // Inform all channel resources that logical port has been closed
            for (auto channelIt : channel_resources_)
            {
                if (channelIt.second->connection_established())
                {
                    rtcp_message_manager_->sendLogicalPortIsClosedRequest(channelIt.second, logicalPort);
                }
            }

            receiver_in_use->cv.wait(scopedLock, [&]() { return receiver_in_use->in_use == false; });
            delete receiver_in_use;
        }
    }

    return bClosed;
}

        std::shared_ptr<TCPChannelResource> channel;

        if (channel_resource != channel_resources_.end())
        {
            channel = channel_resource->second;
        }
        else
        {
            // Create output channel
            logInfo(OpenOutputChannel, "OpenOutputChannel (physical: "
                << IPLocator::getPhysicalPort(locator) << "; logical: "
                << IPLocator::getLogicalPort(locator) << ") @ " << IPLocator::to_string(locator));

            channel.reset(
#if TLS_FOUND
                (configuration()->apply_security) ?
                    static_cast<TCPChannelResource*>(
                        new TCPChannelResourceSecure(this, io_service_, ssl_context_,
                        physical_locator, configuration()->maxMessageSize)) :
#endif
                    static_cast<TCPChannelResource*>(
                        new TCPChannelResourceBasic(this, io_service_, physical_locator,
                        configuration()->maxMessageSize))
                );

            channel_resources_[physical_locator] = channel;
<<<<<<< HEAD
            channel->connect();
        }

        success = true;
        channel->add_logical_port(logical_port);
=======
            channel->connect(channel);
        }

        success = true;
        channel->add_logical_port(logical_port, rtcp_message_manager_.get());
        send_resource_list.emplace_back(
                static_cast<SenderResource*>(new TCPSenderResource(*this, channel)));
    }

    return success;
}

bool TCPTransportInterface::OpenInputChannel(
        const Locator_t& locator,
        TransportReceiverInterface* receiver,
        uint32_t /*maxMsgSize*/)
{
    bool success = false;
    if (IsLocatorSupported(locator))
    {
        uint16_t logicalPort = IPLocator::getLogicalPort(locator);
        if (!is_input_port_open(logicalPort))
        {
            success = true;
            {
                std::unique_lock<std::mutex> scopedLock(sockets_map_mutex_);
                receiver_resources_[logicalPort] = std::pair<TransportReceiverInterface*, ReceiverInUseCV*>
                    (receiver, new ReceiverInUseCV());
            }

            logInfo(RTCP, " OpenInputChannel (physical: " << IPLocator::getPhysicalPort(locator) << "; logical: " << \
                IPLocator::getLogicalPort(locator) << ")");
        }
    }
    return success;
}

<<<<<<< HEAD
void TCPTransportInterface::perform_rtcp_management_thread(
        std::shared_ptr<TCPChannelResource> channel)
=======
void TCPTransportInterface::keep_alive()
>>>>>>> upstream/feature/write_nonblocking
{
    std::unique_lock<std::mutex> scopedLock(sockets_map_mutex_); // Why mutex here?

    for (auto channel_resource : channel_resources_)
    {
        rtcp_message_manager_->sendKeepAliveRequest(channel_resource.second);
    }
    //TODO Check timeout.

    /*
    const TCPTransportDescriptor* config = configuration(); // Keep a copy for us.

    std::chrono::time_point<std::chrono::system_clock> time_now = std::chrono::system_clock::now();
    std::chrono::time_point<std::chrono::system_clock> next_time = time_now +
        std::chrono::milliseconds(config->keep_alive_frequency_ms);
    std::chrono::time_point<std::chrono::system_clock> timeout_time =
        time_now + std::chrono::milliseconds(config->keep_alive_timeout_ms);

<<<<<<< HEAD
/*
    logInfo(RTCP, "START perform_rtcp_management_thread " << IPLocator::toIPv4string(channel->locator()) \
            << ":" << IPLocator::getPhysicalPort(channel->locator()) << " (" \
            << channel->socket()->local_endpoint().address() << ":" \
            << channel->socket()->local_endpoint().port() << "->" \
            << channel->socket()->remote_endpoint().address() << ":" \
            << channel->socket()->remote_endpoint().port() << ")");
*/
=======
>>>>>>> upstream/feature/write_nonblocking
    while (channel && TCPChannelResource::TCPConnectionStatus::TCP_CONNECTED == channel->tcp_connection_status())
    {
        if (channel->connection_established())
        {
            // KeepAlive
            if (config->keep_alive_frequency_ms > 0 && config->keep_alive_timeout_ms > 0)
            {
                time_now = std::chrono::system_clock::now();

                // Keep Alive Management
                if (!channel->waiting_for_keep_alive_ && time_now > next_time)
                {
                    std::unique_lock<std::mutex> scopedLock(sockets_map_mutex_); // Why mutex here?
                    rtcp_message_manager_->sendKeepAliveRequest(channel);
                    channel->waiting_for_keep_alive_ = true;
                    next_time = time_now + std::chrono::milliseconds(config->keep_alive_frequency_ms);
                    timeout_time = time_now + std::chrono::milliseconds(config->keep_alive_timeout_ms);
                }
                else if (channel->waiting_for_keep_alive_ && time_now >= timeout_time)
                {
                    // Disable the socket to erase it after the reception.
                    close_tcp_socket(channel);
<<<<<<< HEAD
                    channel.reset();
=======
>>>>>>> upstream/feature/write_nonblocking
                }
            }
        }

        //TODO Remove
        eClock::my_sleep(100);
    }
    logInfo(RTCP, "End perform_rtcp_management_thread " << channel->locator());
<<<<<<< HEAD
}

void TCPTransportInterface::perform_listen_operation(
        std::shared_ptr<TCPChannelResource> channel)
=======
    */
}

void TCPTransportInterface::perform_listen_operation(
        std::shared_ptr<TCPChannelResource> channel,
        std::weak_ptr<RTCPMessageManager> rtcp_manager)
>>>>>>> upstream/feature/write_nonblocking
{
    Locator_t remote_locator;
    uint16_t logicalPort(0);
    std::shared_ptr<RTCPMessageManager> rtcp_message_manager;

    {
        std::unique_lock<std::mutex> lock(rtcp_message_manager_mutex_);
        rtcp_message_manager = rtcp_manager.lock();
    }

<<<<<<< HEAD
    while (channel && TCPChannelResource::TCPConnectionStatus::TCP_CONNECTED == channel->tcp_connection_status())
=======
    // RTCP Control Message
    if(rtcp_message_manager)
    {
        if (channel->tcp_connection_type() == TCPChannelResource::TCPConnectionType::TCP_CONNECT_TYPE)
        {
            rtcp_message_manager->sendConnectionRequest(channel);
        }
        else
        {
            std::unique_lock<std::mutex> scopedLock(sockets_map_mutex_);
            unbound_channel_resources_.push_back(channel);
            channel->change_status(TCPChannelResource::eConnectionStatus::eWaitingForBind);
            channel->make_thread_joinable();
        }

        std::unique_lock<std::mutex> lock(rtcp_message_manager_mutex_);
        rtcp_message_manager.reset();
        rtcp_message_manager_cv_.notify_one();
    }
    else
    {
        return;
    }


    while (channel && TCPChannelResource::eConnectionStatus::eConnecting < channel->connection_status())
>>>>>>> upstream/feature/write_nonblocking
    {
        // Blocking receive.
        CDRMessage_t& msg = channel->message_buffer();
        CDRMessage::initCDRMsg(&msg);
<<<<<<< HEAD
        if (!Receive(channel, msg.buffer, msg.max_size, msg.length, remote_locator))
=======
        if (!Receive(rtcp_manager, channel, msg.buffer, msg.max_size, msg.length, remote_locator))
>>>>>>> upstream/feature/write_nonblocking
        {
            continue;
        }

        // Processes the data through the CDR Message interface.
        logicalPort = IPLocator::getLogicalPort(remote_locator);
        std::unique_lock<std::mutex> scopedLock(sockets_map_mutex_);
        auto it = receiver_resources_.find(logicalPort);
        //TransportReceiverInterface* receiver = channel->GetMessageReceiver(logicalPort);
        if (it != receiver_resources_.end())
        {
            TransportReceiverInterface* receiver = it->second.first;
            ReceiverInUseCV* receiver_in_use = it->second.second;
            receiver_in_use->in_use = true;
            scopedLock.unlock();
            receiver->OnDataReceived(msg.buffer, msg.length, channel->locator(), remote_locator);
            scopedLock.lock();
            receiver_in_use->in_use = false;
            receiver_in_use->cv.notify_one();
        }
        else
        {
            logWarning(RTCP, "Received Message, but no TransportReceiverInterface attached: " << logicalPort);
        }
    }

    logInfo(RTCP, "End PerformListenOperation " << channel->locator());
}

bool TCPTransportInterface::read_body(
        octet* receive_buffer,
        uint32_t,
        uint32_t* bytes_received,
        std::shared_ptr<TCPChannelResource>& channel,
        std::size_t body_size)
{
    asio::error_code ec;

    *bytes_received = channel->read(receive_buffer, body_size, ec);

    if (ec)
    {
        logWarning(RTCP, "Error reading RTCP body: " << ec.message());
        return false;
    }
    else if (*bytes_received != body_size)
    {
        logError(RTCP, "Bad RTCP body size: " << *bytes_received << " (expected: " << body_size << ")");
        return false;
    }

    return true;
}

/**
* On TCP, we must receive the header (14 Bytes) and then,
* the rest of the message, whose length is on the header.
* TCP Header is transparent to the caller, so receive_buffer
* doesn't include it.
* */
bool TCPTransportInterface::Receive(
<<<<<<< HEAD
=======
        std::weak_ptr<RTCPMessageManager>& rtcp_manager,
>>>>>>> upstream/feature/write_nonblocking
        std::shared_ptr<TCPChannelResource>& channel,
        octet* receive_buffer,
        uint32_t receive_buffer_capacity,
        uint32_t& receive_buffer_size,
        Locator_t& remote_locator)
{
    bool success = false;

    try
    {
<<<<<<< HEAD
        std::unique_lock<std::recursive_mutex> scopedLock(channel->read_mutex());
        // Once mutex is optained, check again, just in case we took it in the little window
        // between disabling and destructor.
=======
>>>>>>> upstream/feature/write_nonblocking
        success = true;

        // Read the header
        //octet header[TCPHEADER_SIZE];
        TCPHeader tcp_header;
        asio::error_code ec;
<<<<<<< HEAD
        //size_t bytes_received = read(*channel->socket(),
        //    asio::buffer(&tcp_header, TCPHeader::getSize()),
        //    transfer_exactly(TCPHeader::getSize()), ec);

        size_t bytes_received = channel->read(reinterpret_cast<octet*>(&tcp_header),
                TCPHeader::size(), ec);

        remote_locator = channel->locator();

        if (bytes_received != TCPHeader::size())
        {
            if (bytes_received > 0)
            {
                logError(RTCP_MSG_IN, "Bad TCP header size: " << bytes_received << " (expected: : "
                        << TCPHeader::size() << ")" << ec.message());
            }
            else
            {
                logWarning(DEBUG, "Error reading TCP header: " << ec.message());
            }
            close_tcp_socket(channel);
            channel.reset();
            success = false;
        }
        else
        {
            // Check RTPC Header
            if (tcp_header.rtcp[0] != 'R'
                    || tcp_header.rtcp[1] != 'T'
                    || tcp_header.rtcp[2] != 'C'
                    || tcp_header.rtcp[3] != 'P')
            {
                logError(RTCP_MSG_IN, "Bad RTCP header identifier, closing connection.");
                close_tcp_socket(channel);
                channel.reset();
                success = false;
            }
            else
            {
                size_t body_size = tcp_header.length - static_cast<uint32_t>(TCPHeader::size());

=======

        size_t bytes_received = channel->read(reinterpret_cast<octet*>(&tcp_header),
                TCPHeader::size(), ec);

        remote_locator = channel->locator();

        if (bytes_received != TCPHeader::size())
        {
            if (bytes_received > 0)
            {
                logError(RTCP_MSG_IN, "Bad TCP header size: " << bytes_received << " (expected: : "
                        << TCPHeader::size() << ")" << ec.message());
            }
            else
            {
                logWarning(DEBUG, "Error reading TCP header: " << ec.message());
            }
            close_tcp_socket(channel);
            success = false;
        }
        else
        {
            // Check RTPC Header
            if (tcp_header.rtcp[0] != 'R'
                    || tcp_header.rtcp[1] != 'T'
                    || tcp_header.rtcp[2] != 'C'
                    || tcp_header.rtcp[3] != 'P')
            {
                logError(RTCP_MSG_IN, "Bad RTCP header identifier, closing connection.");
                close_tcp_socket(channel);
                success = false;
            }
            else
            {
                size_t body_size = tcp_header.length - static_cast<uint32_t>(TCPHeader::size());

>>>>>>> upstream/feature/write_nonblocking
                if (body_size > receive_buffer_capacity)
                {
                    logError(RTCP_MSG_IN, "Size of incoming TCP message is bigger than buffer capacity: "
                            << static_cast<uint32_t>(body_size) << " vs. " << receive_buffer_capacity << ". "
                            << "The full message will be dropped.");
                    success = false;
                    // Drop the message
                    size_t to_read = body_size;
                    size_t read_block = receive_buffer_capacity;
                    uint32_t readed;
                    while (read_block > 0)
                    {
                        read_body(receive_buffer, receive_buffer_capacity, &readed, channel,
                                read_block);
                        to_read -= readed;
                        read_block = (to_read >= receive_buffer_capacity) ? receive_buffer_capacity : to_read;
                    }
                }
                else
                {
                    logInfo(RTCP_MSG_IN, "Received RTCP MSG. Logical Port " << tcp_header.logical_port);
                    success = read_body(receive_buffer, receive_buffer_capacity, &receive_buffer_size,
                            channel, body_size);

                    if (success)
                    {
                        if (configuration()->check_crc
                                && !check_crc(tcp_header, receive_buffer, receive_buffer_size))
                        {
                            logWarning(RTCP_MSG_IN, "Bad TCP header CRC");
                        }
<<<<<<< HEAD

                        if (tcp_header.logical_port == 0)
                        {
                            if (rtcp_message_manager_ != nullptr)
                            {
                                // The channel is not going to be deleted because we lock it for reading.
                                ResponseCode responseCode = rtcp_message_manager_->processRTCPMessage(
                                        channel, receive_buffer, body_size);

                                if (responseCode != RETCODE_OK)
                                {
                                    close_tcp_socket(channel);
                                    channel.reset();
                                }
                                success = false;
                            }
                            else
                            {
                                success = false;
                                close_tcp_socket(channel);
                                channel.reset();
                            }
=======

                        if (tcp_header.logical_port == 0)
                        {
                            std::shared_ptr<RTCPMessageManager> rtcp_message_manager;
                            if(TCPChannelResource::eConnectionStatus::eDisconnected != channel->connection_status())

                            {
                                std::unique_lock<std::mutex> lock(rtcp_message_manager_mutex_);
                                rtcp_message_manager = rtcp_manager.lock();
                            }

                            if (rtcp_message_manager)
                            {
                                // The channel is not going to be deleted because we lock it for reading.
                                ResponseCode responseCode = rtcp_message_manager->processRTCPMessage(
                                        channel, receive_buffer, body_size);

                                if (responseCode != RETCODE_OK)
                                {
                                    close_tcp_socket(channel);
                                }
                                success = false;

                                std::unique_lock<std::mutex> lock(rtcp_message_manager_mutex_);
                                rtcp_message_manager.reset();
                                rtcp_message_manager_cv_.notify_one();
                            }
                            else
                            {
                                success = false;
                                close_tcp_socket(channel);
                            }
>>>>>>> upstream/feature/write_nonblocking

                        }
                        else
                        {
                            IPLocator::setLogicalPort(remote_locator, tcp_header.logical_port);
                            logInfo(RTCP_MSG_IN, "[RECEIVE] From: " << remote_locator \
                                    << " - " << receive_buffer_size << " bytes.");
                        }
                    }
                    // Error message already shown by read_body method.
                }
            }
        }
    }
    catch (const asio::error_code& code)
    {
        if ((code == asio::error::eof) || (code == asio::error::connection_reset))
        {
            // Close the channel
            logError(RTCP_MSG_IN, "ASIO [RECEIVE]: " << code.message());
            //channel->ConnectionLost();
            close_tcp_socket(channel);
<<<<<<< HEAD
            channel.reset();
=======
>>>>>>> upstream/feature/write_nonblocking
        }
        success = false;
    }
    catch (const asio::system_error& error)
    {
        (void)error;
        // Close the channel
        logError(RTCP_MSG_IN, "ASIO SYSTEM_ERROR [RECEIVE]: " << error.what());
        //channel->ConnectionLost();
        close_tcp_socket(channel);
<<<<<<< HEAD
        channel.reset();
=======
>>>>>>> upstream/feature/write_nonblocking
        success = false;
    }

    success = success && receive_buffer_size > 0;

    return success;
}

bool TCPTransportInterface::send(
        const octet* send_buffer,
        uint32_t send_buffer_size,
        std::shared_ptr<TCPChannelResource>& channel,
        const Locator_t& remote_locator)
{
    if (channel->locator() != IPLocator::toPhysicalLocator(remote_locator) ||
            send_buffer_size > configuration()->sendBufferSize)
    {
        logWarning(RTCP, "SEND [RTPS] Failed: Not connect: " << IPLocator::getLogicalPort(remote_locator) \
            << " @ IP: " << IPLocator::toIPv4string(remote_locator));
        return false;
    }

    bool success = false;
<<<<<<< HEAD
=======

    /* TODO Verify when cable is removed
    if(TCPChannelResource::TCPConnectionStatus::TCP_DISCONNECTED == channel->tcp_connection_status() &&
        TCPChannelResource::TCPConnectionType::TCP_ACCEPT_TYPE == channel->tcp_connection_type())
    {
        std::unique_lock<std::mutex> scopedLock(sockets_map_mutex_);
        auto channel_resource = channel_resources_.find(channel->locator());
        if (channel_resource != channel_resources_.end() && channel != channel_resource->second)
        {
            channel = channel_resource->second;
        }
    }*/
>>>>>>> upstream/feature/write_nonblocking

    if (channel->connection_established())
    {
        uint16_t logical_port = IPLocator::getLogicalPort(remote_locator);

        if (channel->is_logical_port_added(logical_port))
        {
<<<<<<< HEAD
            bool bShouldWait = configuration()->wait_for_tcp_negotiation;
            bool bConnected = channel->alive() && channel->connection_established();
            while (bShouldWait && bConnected && !channel->is_logical_port_opened(logical_port))
            {
                bConnected = channel->wait_until_port_is_open_or_connection_is_closed(logical_port);
            }

            if (bConnected && channel->is_logical_port_opened(logical_port))
=======
            if (channel->is_logical_port_opened(logical_port))
>>>>>>> upstream/feature/write_nonblocking
            {
                TCPHeader tcp_header;
                fill_rtcp_header(tcp_header, send_buffer, send_buffer_size, logical_port);

                {
                    asio::error_code ec;
<<<<<<< HEAD
                    uint32_t sent = channel->send(
=======
                    size_t sent = channel->send(
>>>>>>> upstream/feature/write_nonblocking
                        (octet*)&tcp_header,
                        static_cast<uint32_t>(TCPHeader::size()),
                        send_buffer,
                        send_buffer_size,
                        ec);

                    if (sent != static_cast<uint32_t>(TCPHeader::size() + send_buffer_size) || ec)
                    {
                        logWarning(DEBUG, "Failed to send RTCP message (" << sent << " of " <<
                                TCPHeader::size() + send_buffer_size << " b): " << ec.message());
                        success = false;
                    }
                    else
                    {
<<<<<<< HEAD
                        sent = channel->send(send_buffer, send_buffer_size, ec);
                        if (sent != send_buffer_size || ec)
                        {
                            logWarning(DEBUG, "Failed to send body (" << sent << " of " << send_buffer_size << " b): "
                                << ec.message());
                            success = false;
                        }
                        else
                        {
                            success = true;
                        }
=======
                        success = true;
>>>>>>> upstream/feature/write_nonblocking
                    }
                }
            }
        }
        else
        {
<<<<<<< HEAD
            channel->add_logical_port(logical_port);
        }
=======
            channel->add_logical_port(logical_port, rtcp_message_manager_.get());
        }
    }
    else if(TCPChannelResource::TCPConnectionType::TCP_CONNECT_TYPE == channel->tcp_connection_type() &&
            TCPChannelResource::eConnectionStatus::eDisconnected == channel->connection_status())
    {
        channel->set_all_ports_pending();
        channel->connect(channel);
>>>>>>> upstream/feature/write_nonblocking
    }

    return success;
}

LocatorList_t TCPTransportInterface::ShrinkLocatorLists(const std::vector<LocatorList_t>& locatorLists)
{
    LocatorList_t unicastResult;
    for (const LocatorList_t& locatorList : locatorLists)
    {
        LocatorListConstIterator it = locatorList.begin();
        LocatorList_t pendingUnicast;

        while (it != locatorList.end())
        {
            assert((*it).kind == transport_kind_);

            // Check is local interface.
            auto localInterface = current_interfaces_.begin();
            for (; localInterface != current_interfaces_.end(); ++localInterface)
            {
                if (compare_locator_ip(localInterface->locator, *it))
                {
                    // Loopback locator
                    Locator_t loopbackLocator;
                    fill_local_ip(loopbackLocator);
                    IPLocator::setPhysicalPort(loopbackLocator, IPLocator::getPhysicalPort(*it));
                    IPLocator::setLogicalPort(loopbackLocator, IPLocator::getLogicalPort(*it));
                    pendingUnicast.push_back(loopbackLocator);
                    break;
                }
            }

            if (localInterface == current_interfaces_.end())
                pendingUnicast.push_back(*it);

            ++it;
        }

        unicastResult.push_back(pendingUnicast);
    }

    LocatorList_t result(std::move(unicastResult));
    return result;
}

void TCPTransportInterface::SocketAccepted(
        const std::shared_ptr<TCPAcceptorBasic>& acceptor,
        const asio::error_code& error)
{
    if (!error.value())
    {
<<<<<<< HEAD
        std::unique_lock<std::mutex> scopedLock(sockets_map_mutex_);
        if (socket_acceptors_.find(IPLocator::getPhysicalPort(acceptor_locator)) != socket_acceptors_.end())
        {
            // Store the new connection.
            std::shared_ptr<TCPChannelResource> channel(new TCPChannelResourceBasic(this, rtcp_message_manager_,
                io_service_, acceptor->move_socket(), configuration()->maxMessageSize));

            channel->set_options(configuration());

            channel->thread(new std::thread(&TCPTransportInterface::perform_listen_operation, this,
                channel));
            channel->rtcp_thread(new std::thread(&TCPTransportInterface::perform_rtcp_management_thread,
                this, channel));
=======
        // Store the new connection.
        std::shared_ptr<TCPChannelResource> channel(new TCPChannelResourceBasic(this,
            io_service_, acceptor->move_socket(), configuration()->maxMessageSize));
>>>>>>> upstream/feature/write_nonblocking

        channel->set_options(configuration());
        std::weak_ptr<RTCPMessageManager> rtcp_manager_weak_ptr = rtcp_message_manager_;
        channel->thread(std::thread(&TCPTransportInterface::perform_listen_operation, this,
            channel, rtcp_manager_weak_ptr), false);

<<<<<<< HEAD
            logInfo(RTCP, " Accepted connection (local: " << IPLocator::to_string(acceptor_locator)
                << ", remote: " << channel->remote_endpoint().address()
                << ":" << channel->remote_endpoint().port() << ")");
        }
        else
        {
            logError(RTPC, "Incomming connection from unknown Acceptor: "
                << IPLocator::getPhysicalPort(acceptor_locator));
            return;
        }
=======
        logInfo(RTCP, " Accepted connection (local: " << IPLocator::to_string(acceptor->locator())
            << ", remote: " << channel->remote_endpoint().address()
            << ":" << channel->remote_endpoint().port() << ")");
>>>>>>> upstream/feature/write_nonblocking
    }
    else
    {
        logInfo(RTCP, " Accepting connection (" << error.message() << ")");
        eClock::my_sleep(200); // Wait a little to accept again.
    }

    if (error.value() != eSocketErrorCodes::eConnectionAborted) // Operation Aborted
    {
        acceptor->accept(this, acceptor);
    }
}

#if TLS_FOUND
void TCPTransportInterface::SecureSocketAccepted(
        const std::shared_ptr<TCPAcceptorSecure>& acceptor,
        const asio::error_code& error)
{
    if (!error.value())
    {
<<<<<<< HEAD
        std::unique_lock<std::mutex> scopedLock(sockets_map_mutex_);
        if (socket_acceptors_.find(IPLocator::getPhysicalPort(acceptor_locator)) != socket_acceptors_.end())
        {
            // Store the new connection.
            std::shared_ptr<TCPChannelResource> secure_channel(new TCPChannelResourceSecure(this, rtcp_message_manager_,
                io_service_, ssl_context_, acceptor->move_socket(), configuration()->maxMessageSize));

            secure_channel->set_options(configuration());
            secure_channel->thread(new std::thread(&TCPTransportInterface::perform_listen_operation, this,
                secure_channel));
            secure_channel->rtcp_thread(new std::thread(&TCPTransportInterface::perform_rtcp_management_thread,
                this, secure_channel));


            logInfo(RTCP, " Accepted connection (local: " << IPLocator::to_string(acceptor_locator)
                << ", remote: " << secure_channel->remote_endpoint().address()
                << ":" << secure_channel->remote_endpoint().port() << ")");
        }
        else
        {
            logError(RTPC, "Incomming connection from unknown Acceptor: "
                << IPLocator::getPhysicalPort(acceptor_locator));
            return;
        }
=======
        // Store the new connection.
        std::shared_ptr<TCPChannelResource> secure_channel(new TCPChannelResourceSecure(this,
            io_service_, ssl_context_, acceptor->move_socket(), configuration()->maxMessageSize));

        secure_channel->set_options(configuration());
        std::weak_ptr<RTCPMessageManager> rtcp_manager_weak_ptr = rtcp_message_manager_;
        secure_channel->thread(std::thread(&TCPTransportInterface::perform_listen_operation, this,
            secure_channel, rtcp_manager_weak_ptr), false);

        logInfo(RTCP, " Accepted connection (local: " << IPLocator::to_string(acceptor->locator())
            << ", remote: " << secure_channel->remote_endpoint().address()
            << ":" << secure_channel->remote_endpoint().port() << ")");
>>>>>>> upstream/feature/write_nonblocking
    }
    else
    {
        logError(RTCP, " Accepting connection failed (" << error.message() << ")");
        eClock::my_sleep(200); // Wait a little to accept again.
    }

    if (error.value() != eSocketErrorCodes::eConnectionAborted) // Operation Aborted
    {
        // Accept new connections for the same port. Could be not found when exiting.
        acceptor->accept(this, acceptor, ssl_context_);
    }
    else
    {
        logError(RTCP_TLS, "Connection aborted in acceptor: " << error.message());
    }

}
#endif

//TODO Passh the shared_ptr
void TCPTransportInterface::SocketConnected(
        const std::shared_ptr<TCPChannelResource>& channel,
        const asio::error_code& error)
{
<<<<<<< HEAD
    std::shared_ptr<TCPChannelResource> channel;

    {
        std::unique_lock<std::mutex> scopedLock(sockets_map_mutex_);
        auto it = channel_resources_.find(IPLocator::toPhysicalLocator(locator));
        if (it != channel_resources_.end())
        {
            channel = it->second;
        }

        if(channel && error.value() == 0)
=======
    if (!error)
    {
        try
>>>>>>> upstream/feature/write_nonblocking
        {
            if(TCPChannelResource::eConnectionStatus::eDisconnected < channel->connection_status())
            {
<<<<<<< HEAD
                channel->tcp_connected();
                channel->set_options(configuration());

                channel->thread(
                    new std::thread(&TCPTransportInterface::perform_listen_operation, this, channel));
                channel->rtcp_thread(
                    new std::thread(&TCPTransportInterface::perform_rtcp_management_thread, this, channel));

                // RTCP Control Message
                rtcp_message_manager_->sendConnectionRequest(channel);
            }
            catch (asio::system_error const& /*e*/)
            {
                /*
                (void)e;
                logInfo(RTCP_MSG_OUT, "TCPTransport Error establishing the connection at port:("
                    << IPLocator::getPhysicalPort(locator) << ")" << " with msg:" << e.what());
                CloseOutputChannel(locator);
                */
=======
                channel->set_options(configuration());

                std::weak_ptr<RTCPMessageManager> rtcp_manager_weak_ptr = rtcp_message_manager_;
                channel->thread(std::thread(&TCPTransportInterface::perform_listen_operation, this,
                        channel, rtcp_manager_weak_ptr));
>>>>>>> upstream/feature/write_nonblocking
            }
        }
<<<<<<< HEAD
    }

    // If we get here, the error isn't zero.
    if (channel)
    {
        if (error.value() == asio::error::basic_errors::connection_refused)
        {
            // Wait a little before try again to avoid exhaust file descriptors in some systems
            // TODO Study. This blocks the asio thread
            eClock::my_sleep(200);
        }
        else if (error.value() == asio::error::basic_errors::connection_reset ||
                error.value() == asio::error::basic_errors::connection_aborted)
        {
            // Connection was closed by the remote. Wait a little more to retry.
            logError(RTCP_TLS, "Connection broken: " << error.message());
            eClock::my_sleep(1000);
        }
        else
        {
            logError(RTCP_TLS, error.message());
        }
        channel->change_status(TCPChannelResource::eConnectionStatus::eDisconnected);
        channel->set_all_ports_pending();
        channel->connect();
=======
        catch (asio::system_error const& /*e*/)
        {
            /*
            (void)e;
            logInfo(RTCP_MSG_OUT, "TCPTransport Error establishing the connection at port:("
                << IPLocator::getPhysicalPort(locator) << ")" << " with msg:" << e.what());
            CloseOutputChannel(locator);
            */
        }
    }
    else
    {
        channel->disable();
>>>>>>> upstream/feature/write_nonblocking
    }
}

bool TCPTransportInterface::getDefaultMetatrafficMulticastLocators(
        LocatorList_t &,
        uint32_t ) const
{
    // TCP doesn't have multicast support
    return true;
}

bool TCPTransportInterface::getDefaultMetatrafficUnicastLocators(LocatorList_t &locators,
    uint32_t metatraffic_unicast_port) const
{
    Locator_t locator;
    locator.kind = transport_kind_;
    locator.set_Invalid_Address();
    fillMetatrafficUnicastLocator(locator, metatraffic_unicast_port);
    locators.push_back(locator);
    return true;
}

bool TCPTransportInterface::getDefaultUnicastLocators(
        LocatorList_t &locators,
        uint32_t unicast_port) const
{
    Locator_t locator;
    locator.kind = transport_kind_;
    locator.set_Invalid_Address();
    fillUnicastLocator(locator, unicast_port);
    locators.push_back(locator);
    return true;
}

bool TCPTransportInterface::fillMetatrafficMulticastLocator(
        Locator_t &,
        uint32_t) const
{
    // TCP doesn't have multicast support
    return true;
}

bool TCPTransportInterface::fillMetatrafficUnicastLocator(
        Locator_t &locator,
        uint32_t metatraffic_unicast_port) const
{
    if (IPLocator::getPhysicalPort(locator.port) == 0)
    {
        const TCPTransportDescriptor* config = configuration();
        if (config != nullptr)
        {
            if (!config->listening_ports.empty())
            {
                IPLocator::setPhysicalPort(locator, *(config->listening_ports.begin()));
            }
            else
            {
                IPLocator::setPhysicalPort(locator, static_cast<uint16_t>(System::GetPID()));
            }
        }
    }

    if (IPLocator::getLogicalPort(locator) == 0)
    {
        IPLocator::setLogicalPort(locator, static_cast<uint16_t>(metatraffic_unicast_port));
    }

    // TODO: Add WAN address

    return true;
}

bool TCPTransportInterface::configureInitialPeerLocator(
        Locator_t &locator,
        const PortParameters &port_params,
        uint32_t domainId,
        LocatorList_t& list) const
{
    if(IPLocator::getPhysicalPort(locator) == 0)
    {
        for(uint32_t i = 0; i < configuration()->maxInitialPeersRange; ++i)
        {
            Locator_t auxloc(locator);
            auxloc.port = static_cast<uint16_t>(port_params.getUnicastPort(domainId, i));

            if (IPLocator::getLogicalPort(locator) == 0)
            {
                IPLocator::setLogicalPort(auxloc, static_cast<uint16_t>(port_params.getUnicastPort(domainId, i)));
            }

            list.push_back(auxloc);
        }
    }
    else
    {
        if (IPLocator::getLogicalPort(locator) == 0)
        {
            for(uint32_t i = 0; i < configuration()->maxInitialPeersRange; ++i)
            {
                Locator_t auxloc(locator);
                IPLocator::setLogicalPort(auxloc, static_cast<uint16_t>(port_params.getUnicastPort(domainId, i)));
                list.push_back(auxloc);
            }
        }
        else
        {
            list.push_back(locator);
        }
    }

    return true;
}

bool TCPTransportInterface::fillUnicastLocator(
        Locator_t &locator,
        uint32_t well_known_port) const
{
    if (IPLocator::getPhysicalPort(locator.port) == 0)
    {
        const TCPTransportDescriptor* config = configuration();
        if (config != nullptr)
        {
            if (!config->listening_ports.empty())
            {
                IPLocator::setPhysicalPort(locator, *(config->listening_ports.begin()));
            }
            else
            {
                IPLocator::setPhysicalPort(locator, static_cast<uint16_t>(System::GetPID()));
            }
        }
    }

    if (IPLocator::getLogicalPort(locator) == 0)
    {
        IPLocator::setLogicalPort(locator, static_cast<uint16_t>(well_known_port));
    }

    // TODO: Add WAN address

    return true;
}

void TCPTransportInterface::shutdown()
{
}

void TCPTransportInterface::apply_tls_config()
{
#if TLS_FOUND
    const TCPTransportDescriptor* descriptor = configuration();
    if (descriptor->apply_security)
    {
        ssl_context_.set_verify_callback([](bool preverified, ssl::verify_context&)
        {
            return preverified;
        });

        const TCPTransportDescriptor::TLSConfig* config = &descriptor->tls_config;
        using TLSOptions = TCPTransportDescriptor::TLSConfig::TLSOptions;

        if (!config->password.empty())
        {
            ssl_context_.set_password_callback(std::bind(&TCPTransportInterface::get_password, this));
        }

        if (!config->verify_file.empty())
        {
            ssl_context_.load_verify_file(config->verify_file);
        }

        if (!config->cert_chain_file.empty())
        {
            ssl_context_.use_certificate_chain_file(config->cert_chain_file);
        }

        if (!config->private_key_file.empty())
        {
            ssl_context_.use_private_key_file(config->private_key_file, ssl::context::pem);
        }

        if (!config->tmp_dh_file.empty())
        {
            ssl_context_.use_tmp_dh_file(config->tmp_dh_file);
        }

        if (!config->verify_paths.empty())
        {
            for (const std::string& path : config->verify_paths)
            {
                ssl_context_.add_verify_path(path);
            }
        }

        if (config->default_verify_path)
        {
            ssl_context_.set_default_verify_paths();
        }

        if (config->verify_depth >= 0)
        {
            ssl_context_.set_verify_depth(config->verify_depth);
        }

        if (!config->rsa_private_key_file.empty())
        {
            ssl_context_.use_private_key_file(config->rsa_private_key_file, ssl::context::pem);
        }

        if (config->options != TLSOptions::NONE)
        {
            uint32_t options = 0;

            if (config->get_option(TLSOptions::DEFAULT_WORKAROUNDS))
            {
                options |= ssl::context::default_workarounds;
            }

            if (config->get_option(TLSOptions::NO_COMPRESSION))
            {
                options |= ssl::context::no_compression;
            }

            if (config->get_option(TLSOptions::NO_SSLV2))
            {
                options |= ssl::context::no_sslv2;
            }

            if (config->get_option(TLSOptions::NO_SSLV3))
            {
                options |= ssl::context::no_sslv3;
            }

            if (config->get_option(TLSOptions::NO_TLSV1))
            {
                options |= ssl::context::no_tlsv1;
            }

            if (config->get_option(TLSOptions::NO_TLSV1_1))
            {
                options |= ssl::context::no_tlsv1_1;
            }

            if (config->get_option(TLSOptions::NO_TLSV1_2))
            {
                options |= ssl::context::no_tlsv1_2;
            }

#if ASIO_VERSION >= 106900 // no_tlsv1_3 added in asio 1.69
            if (config->get_option(TLSOptions::NO_TLSV1_3))
            {
                options |= ssl::context::no_tlsv1_3;
            }
#endif

            if (config->get_option(TLSOptions::SINGLE_DH_USE))
            {
                options |= ssl::context::single_dh_use;
            }

            ssl_context_.set_options(options);
        }
    }
#endif
}

std::string TCPTransportInterface::get_password() const
{
    return configuration()->tls_config.password;
}

} // namespace rtps
} // namespace fastrtps
} // namespace eprosima

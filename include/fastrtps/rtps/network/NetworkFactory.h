// Copyright 2016 Proyectos y Sistemas de Mantenimiento SL (eProsima).
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

#ifndef NETWORK_FACTORY_HPP
#define NETWORK_FACTORY_HPP

#include <fastrtps/transport/TransportInterface.h>
#include <fastrtps/rtps/network/ReceiverResource.h>
#include <fastrtps/rtps/network/SenderResource.h>
#include <fastrtps/rtps/messages/MessageReceiver.h>
#include <vector>
#include <memory>

namespace eprosima{
namespace fastrtps{
namespace rtps{

class RTPSParticipantAttributes;

/**
 * Provides the FastRTPS library with abstract resources, which
 * in turn manage the SEND and RECEIVE operations over some transport.
 * Once a transport is registered, it becomes invisible to the library
 * and is abstracted away for good.
 * @ingroup NETWORK_MODULE.
 */
class NetworkFactory
{
    public:

        NetworkFactory();

        /**
         * Allows registration of a transport statically, by specifying the transport type and
         * its associated descriptor type. This is particularly useful for user-defined transports.
         */
        template<class T, class D>
            void RegisterTransport(const D& descriptor)
            {
                std::unique_ptr<T> transport(new T(descriptor));
                if(transport->init())
                    mRegisteredTransports.emplace_back(std::move(transport));
            }

        /**
        * Allows registration of a transport dynamically. Only the transports built into FastRTPS
        * are supported here (although it can be easily extended at NetworkFactory.cpp)
        * @param descriptor Structure that defines all initial configuration for a given transport.
        */
        bool RegisterTransport(const TransportDescriptorInterface* descriptor);

        /**
         * Walks over the list of transports, opening every possible channel that can send through
         * the given locator and returning a vector of Sender Resources associated with it.
         * @param local Locator through which to send.
         */
        bool build_send_resources(
                SendResourceList&,
                const Locator_t& locator);

        /**
         * Walks over the list of transports, opening every possible channel that we can listen to
         * from the given locator, and returns a vector of Receiver Resources for this goal.
         * @param local Locator from which to listen.
         */
        bool BuildReceiverResources(Locator_t& local, uint32_t maxMsgSize,
            std::vector<std::shared_ptr<ReceiverResource>>& returned_resources_list);

        void NormalizeLocators(LocatorList_t& locators);

        LocatorList_t ShrinkLocatorLists(const std::vector<LocatorList_t>& locatorLists);

        bool is_local_locator(const Locator_t& locator) const;

        size_t numberOfRegisteredTransports() const;

        uint32_t get_max_message_size_between_transports() const { return maxMessageSizeBetweenTransports_; }

        uint32_t get_min_send_buffer_size() { return minSendBufferSize_; }

        /**
         * Fills ret_locators with the list of all possible locators in the local machine at the given
         * physical_port of the locator_kind.
         * Return if found any.
         * */
        bool generate_locators(uint16_t physical_port, int locator_kind, LocatorList_t &ret_locators);

        /**
         * For each transport, ask for their default output locators.
         * */
        void GetDefaultOutputLocators(LocatorList_t &defaultLocators);

        /**
         * Adds locators to the metatraffic multicast list.
         * */
        bool getDefaultMetatrafficMulticastLocators(LocatorList_t &locators, uint32_t metatraffic_multicast_port) const;

        /**
        * Adds locators to the metatraffic unicast list.
        * */
        bool getDefaultMetatrafficUnicastLocators(LocatorList_t &locators, uint32_t metatraffic_unicast_port) const;

        /**
         * Fills the locator with the metatraffic multicast configuration.
         * */
        bool fillMetatrafficMulticastLocator(Locator_t &locator, uint32_t metatraffic_multicast_port) const;

        /**
         * Fills the locator with the metatraffic unicast configuration.
         * */
        bool fillMetatrafficUnicastLocator(Locator_t &locator, uint32_t metatraffic_unicast_port) const;

        /**
         * Configures the locator with the initial peer configuration.
         * */
        bool configureInitialPeerLocator(Locator_t &locator, RTPSParticipantAttributes& m_att) const;

        /**
         * Adds locators to the default unicast configuration.
         * */
        bool getDefaultUnicastLocators(LocatorList_t &locators, const RTPSParticipantAttributes& m_att) const;

        /**
         * Fills the locator with the default unicast configuration.
         * */
        bool fillDefaultUnicastLocator(Locator_t &locator, const RTPSParticipantAttributes& m_att) const;

        /**
         * Shutdown method to close the connections of the transports.
        */
        void Shutdown();

    private:

        std::vector<std::unique_ptr<TransportInterface> > mRegisteredTransports;

        uint32_t maxMessageSizeBetweenTransports_;

        uint32_t minSendBufferSize_;

        /**
         * Calculates well-known ports.
        */
        uint16_t calculateWellKnownPort(const RTPSParticipantAttributes& att) const;
};

} // namespace rtps
} // namespace fastrtps
} // namespace eprosima

#endif

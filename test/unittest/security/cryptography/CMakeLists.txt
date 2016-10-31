# Copyright 2016 Proyectos y Sistemas de Mantenimiento SL (eProsima).
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#     http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.

if(NOT ((MSVC OR MSVC_IDE) AND EPROSIMA_INSTALLER))
    include(${PROJECT_SOURCE_DIR}/cmake/dev/gtest.cmake)
    check_gtest()

    if(GTEST_FOUND)
        if(WIN32)
            add_definitions(-D_WIN32_WINNT=0x0601)
        endif()

        set(COMMON_SOURCES_CRYPTO_PLUGIN_TEST_SOURCE
            ${PROJECT_SOURCE_DIR}/src/cpp/log/Log.cpp
            ${PROJECT_SOURCE_DIR}/src/cpp/log/StdoutConsumer.cpp
            ${PROJECT_SOURCE_DIR}/src/cpp/rtps/attributes/PropertyPolicy.cpp
            ${PROJECT_SOURCE_DIR}/src/cpp/rtps/common/Token.cpp
            ${PROJECT_SOURCE_DIR}/src/cpp/rtps/exceptions/Exception.cpp
            ${PROJECT_SOURCE_DIR}/src/cpp/rtps/security/exceptions/SecurityException.cpp
            ${PROJECT_SOURCE_DIR}/src/cpp/rtps/security/common/SharedSecretHandle.cpp
            )

        add_executable(BuiltinAESGCMGMAC ${COMMON_SOURCES_CRYPTO_PLUGIN_TEST_SOURCE}
            ${PROJECT_SOURCE_DIR}/src/cpp/security/cryptography/AESGCMGMAC.cpp
            ${PROJECT_SOURCE_DIR}/src/cpp/security/cryptography/AESGCMGMAC_KeyExchange.cpp
            ${PROJECT_SOURCE_DIR}/src/cpp/security/cryptography/AESGCMGMAC_KeyFactory.cpp
            ${PROJECT_SOURCE_DIR}/src/cpp/security/cryptography/AESGCMGMAC_Transform.cpp
            ${PROJECT_SOURCE_DIR}/src/cpp/security/cryptography/AESGCMGMAC_Types.cpp
            ${PROJECT_SOURCE_DIR}/src/cpp/security/authentication/PKIIdentityHandle.cpp
            ${PROJECT_SOURCE_DIR}/src/cpp/security/access/mockAccessHandle.cpp

            ${CMAKE_CURRENT_SOURCE_DIR}/builtinAESGCMGMACTests.cpp)
        add_gtest(BuiltinAESGCMGMAC ${COMMON_SOURCES_CRYPTO_PLUGIN_TEST_SOURCE}
            ${PROJECT_SOURCE_DIR}/src/cpp/security/cryptography/AESGCMGMAC.cpp
            ${PROJECT_SOURCE_DIR}/src/cpp/security/cryptography/AESGCMGMAC_KeyExchange.cpp
            ${PROJECT_SOURCE_DIR}/src/cpp/security/cryptography/AESGCMGMAC_KeyFactory.cpp
            ${PROJECT_SOURCE_DIR}/src/cpp/security/cryptography/AESGCMGMAC_Transform.cpp
            ${PROJECT_SOURCE_DIR}/src/cpp/security/cryptography/AESGCMGMAC_Types.cpp
            ${CMAKE_CURRENT_SOURCE_DIR}/BuiltinAESGCMGMACTests.cpp
            ENVIRONMENT "CERTS_PATH=${PROJECT_SOURCE_DIR}/test/certs")
        target_include_directories(BuiltinAESGCMGMAC PRIVATE
            ${GTEST_INCLUDE_DIRS}
            ${PROJECT_SOURCE_DIR}/include ${PROJECT_BINARY_DIR}/include/${PROJECT_NAME})
        target_link_libraries(BuiltinAESGCMGMAC ${GTEST_LIBRARIES} ${OPENSSL_LIBRARIES})
    endif()
endif()
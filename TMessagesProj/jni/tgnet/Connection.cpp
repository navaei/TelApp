/*
 * This is the source code of tgnet library v. 1.1
 * It is licensed under GNU GPL v. 2 or later.
 * You should have received a copy of the license in this archive (see LICENSE).
 *
 * Copyright Nikolai Kudashov, 2015-2018.
 */

#include <openssl/rand.h>
#include <stdlib.h>
#include <cstring>
#include <openssl/sha.h>
#include "Connection.h"
#include "ConnectionsManager.h"
#include "BuffersStorage.h"
#include "FileLog.h"
#include "Timer.h"
#include "Datacenter.h"
#include "NativeByteBuffer.h"
#include "ByteArray.h"

thread_local static uint32_t lastConnectionToken = 1;

Connection::Connection(Datacenter *datacenter, ConnectionType type, int8_t num) : ConnectionSession(datacenter->instanceNum), ConnectionSocket(datacenter->instanceNum) {
    currentDatacenter = datacenter;
    connectionNum = num;
    connectionType = type;
    genereateNewSessionId();
    connectionState = TcpConnectionStageIdle;
    reconnectTimer = new Timer(datacenter->instanceNum, [&] {
        reconnectTimer->stop();
        waitForReconnectTimer = false;
        connect();
    });
}

Connection::~Connection() {
    if (reconnectTimer != nullptr) {
        reconnectTimer->stop();
        delete reconnectTimer;
        reconnectTimer = nullptr;
    }
}

void Connection::suspendConnection() {
    suspendConnection(false);
}

void Connection::suspendConnection(bool idle) {
    reconnectTimer->stop();
    waitForReconnectTimer = false;
    if (connectionState == TcpConnectionStageIdle || connectionState == TcpConnectionStageSuspended) {
        return;
    }
    if (LOGS_ENABLED) DEBUG_D("connection(%p, account%u, dc%u, type %d) suspend", this, currentDatacenter->instanceNum, currentDatacenter->getDatacenterId(), connectionType);
    connectionState = idle ? TcpConnectionStageIdle : TcpConnectionStageSuspended;
    dropConnection();
    ConnectionsManager::getInstance(currentDatacenter->instanceNum).onConnectionClosed(this, 0);
    firstPacketSent = false;
    if (restOfTheData != nullptr) {
        restOfTheData->reuse();
        restOfTheData = nullptr;
    }
    lastPacketLength = 0;
    connectionToken = 0;
    wasConnected = false;
}

void Connection::onReceivedData(NativeByteBuffer *buffer) {
    //AES_ctr128_encrypt(buffer->bytes(), buffer->bytes(), buffer->limit(), &decryptKey, decryptIv, decryptCount, &decryptNum);

    failedConnectionCount = 0;

//    if (connectionType == ConnectionTypeGeneric || connectionType == ConnectionTypeTemp || connectionType == ConnectionTypeGenericMedia) {
//        receivedDataAmount += buffer->limit();
//        if (receivedDataAmount >= 512 * 1024) {
//            if (currentTimeout > 4) {
//                currentTimeout -= 2;
//                setTimeout(currentTimeout);
//            }
//            receivedDataAmount = 0;
//        }
//    }

    NativeByteBuffer *parseLaterBuffer = nullptr;
    if (restOfTheData != nullptr) {
        if (lastPacketLength == 0) {
            if (restOfTheData->capacity() - restOfTheData->position() >= buffer->limit()) {
                restOfTheData->limit(restOfTheData->position() + buffer->limit());
                restOfTheData->writeBytes(buffer);
                buffer = restOfTheData;
            } else {
                NativeByteBuffer *newBuffer = BuffersStorage::getInstance().getFreeBuffer(restOfTheData->limit() + buffer->limit());
                restOfTheData->rewind();
                newBuffer->writeBytes(restOfTheData);
                newBuffer->writeBytes(buffer);
                buffer = newBuffer;
                restOfTheData->reuse();
                restOfTheData = newBuffer;
            }
        } else {
            uint32_t len;
            if (lastPacketLength - restOfTheData->position() <= buffer->limit()) {
                len = lastPacketLength - restOfTheData->position();
            } else {
                len = buffer->limit();
            }
            uint32_t oldLimit = buffer->limit();
            buffer->limit(len);
            restOfTheData->writeBytes(buffer);
            buffer->limit(oldLimit);
            if (restOfTheData->position() == lastPacketLength) {
                parseLaterBuffer = buffer->hasRemaining() ? buffer : nullptr;
                buffer = restOfTheData;
            } else {
                if (LOGS_ENABLED) DEBUG_D("connection(%p, account%u, dc%u, type %d) received packet size less(%u) then message size(%u)", this, currentDatacenter->instanceNum, currentDatacenter->getDatacenterId(), connectionType, restOfTheData->position(), lastPacketLength);
                return;
            }
        }
    }

    buffer->rewind();

    while (buffer->hasRemaining()) {
        if (!hasSomeDataSinceLastConnect) {
            currentDatacenter->storeCurrentAddressAndPortNum();
            isTryingNextPort = false;
            if (connectionType == ConnectionTypeProxy) {
                setTimeout(5);
            } else if (connectionType == ConnectionTypePush) {
                setTimeout(60 * 15);
            } else if (connectionType == ConnectionTypeUpload) {
                if (ConnectionsManager::getInstance(currentDatacenter->instanceNum).networkSlow) {
                    setTimeout(40);
                } else {
                    setTimeout(25);
                }
            } else if (connectionType == ConnectionTypeDownload) {
                setTimeout(25);
            } else {
                setTimeout(currentTimeout);
            }
        }
        hasSomeDataSinceLastConnect = true;

        //TODO must delete
        uint32_t mark = buffer->position();
        uint32_t currentPacketLength = buffer->readUint32(nullptr);
        uint32_t sequenceFromServer = buffer->readUint32(nullptr);

//        uint32_t currentPacketLength = 0;
//        uint32_t mark = buffer->position();
//        uint32_t len;

//        if (currentProtocolType == ProtocolTypeEF) {
//            uint8_t fByte = buffer->readByte(nullptr);
//
//            if ((fByte & (1 << 7)) != 0) {
//                buffer->position(mark);
//                if (buffer->remaining() < 4) {
//                    NativeByteBuffer *reuseLater = restOfTheData;
//                    restOfTheData = BuffersStorage::getInstance().getFreeBuffer(16384);
//                    restOfTheData->writeBytes(buffer);
//                    restOfTheData->limit(restOfTheData->position());
//                    lastPacketLength = 0;
//                    if (reuseLater != nullptr) {
//                        reuseLater->reuse();
//                    }
//                    break;
//                }
//                int32_t ackId = buffer->readBigInt32(nullptr) & (~(1 << 31));
//                ConnectionsManager::getInstance(currentDatacenter->instanceNum).onConnectionQuickAckReceived(this, ackId);
//                continue;
//            }
//
//            if (fByte != 0x7f) {
//                currentPacketLength = ((uint32_t) fByte) * 4;
//            } else {
//                buffer->position(mark);
//                if (buffer->remaining() < 4) {
//                    if (restOfTheData == nullptr || (restOfTheData != nullptr && restOfTheData->position() != 0)) {
//                        NativeByteBuffer *reuseLater = restOfTheData;
//                        restOfTheData = BuffersStorage::getInstance().getFreeBuffer(16384);
//                        restOfTheData->writeBytes(buffer);
//                        restOfTheData->limit(restOfTheData->position());
//                        lastPacketLength = 0;
//                        if (reuseLater != nullptr) {
//                            reuseLater->reuse();
//                        }
//                    } else {
//                        restOfTheData->position(restOfTheData->limit());
//                    }
//                    break;
//                }
//                currentPacketLength = ((uint32_t) buffer->readInt32(nullptr) >> 8) * 4;
//            }
//
//            len = currentPacketLength + (fByte != 0x7f ? 1 : 4);
//        } else {
//            if (buffer->remaining() < 4) {
//                if (restOfTheData == nullptr || (restOfTheData != nullptr && restOfTheData->position() != 0)) {
//                    NativeByteBuffer *reuseLater = restOfTheData;
//                    restOfTheData = BuffersStorage::getInstance().getFreeBuffer(16384);
//                    restOfTheData->writeBytes(buffer);
//                    restOfTheData->limit(restOfTheData->position());
//                    lastPacketLength = 0;
//                    if (reuseLater != nullptr) {
//                        reuseLater->reuse();
//                    }
//                } else {
//                    restOfTheData->position(restOfTheData->limit());
//                }
//                break;
//            }
//
//            uint32_t fInt = buffer->readUint32(nullptr);
//
//            if ((fInt & (0x80000000)) != 0) {
//                ConnectionsManager::getInstance(currentDatacenter->instanceNum).onConnectionQuickAckReceived(this, fInt & (~(1 << 31)));
//                continue;
//            }
//
//            currentPacketLength = fInt;
//            len = currentPacketLength + 4;
//        }

        if (currentProtocolType != ProtocolTypeDD && currentPacketLength % 4 != 0 || currentPacketLength > 2 * 1024 * 1024) {
            if (LOGS_ENABLED) DEBUG_D("connection(%p, account%u, dc%u, type %d) received invalid packet length", this, currentDatacenter->instanceNum, currentDatacenter->getDatacenterId(), connectionType);
            reconnect();
            return;
        }

        if (currentPacketLength < buffer->remaining()) {
            if (LOGS_ENABLED) DEBUG_D("connection(%p, account%u, dc%u, type %d) received message len %u but packet larger %u", this, currentDatacenter->instanceNum, currentDatacenter->getDatacenterId(), connectionType, currentPacketLength, buffer->remaining());
        } else if (currentPacketLength == buffer->remaining()) {
            if (LOGS_ENABLED) DEBUG_D("connection(%p, account%u, dc%u, type %d) received message len %u equal to packet size", this, currentDatacenter->instanceNum, currentDatacenter->getDatacenterId(), connectionType, currentPacketLength);
        } else {
            if (LOGS_ENABLED) DEBUG_D("connection(%p, account%u, dc%u, type %d) received packet size less(%u) then message size(%u)", this, currentDatacenter->instanceNum, currentDatacenter->getDatacenterId(), connectionType, buffer->remaining(), currentPacketLength);

            NativeByteBuffer *reuseLater = nullptr;
            //TODO must delete
            uint32_t len = currentPacketLength + 8;

            if (restOfTheData != nullptr && restOfTheData->capacity() < len) {
                reuseLater = restOfTheData;
                restOfTheData = nullptr;
            }
            if (restOfTheData == nullptr) {
                buffer->position(mark);
                restOfTheData = BuffersStorage::getInstance().getFreeBuffer(len);
                restOfTheData->writeBytes(buffer);
            } else {
                restOfTheData->position(restOfTheData->limit());
                restOfTheData->limit(len);
            }
            lastPacketLength = len;
            if (reuseLater != nullptr) {
                reuseLater->reuse();
            }
            return;
        }

        uint32_t old = buffer->limit();
        buffer->limit(buffer->position() + currentPacketLength);
        ConnectionsManager::getInstance(currentDatacenter->instanceNum).onConnectionDataReceived(this, buffer, currentPacketLength);
        buffer->position(buffer->limit());
        buffer->limit(old);

        if (restOfTheData != nullptr) {
            if ((lastPacketLength != 0 && restOfTheData->position() == lastPacketLength) || (lastPacketLength == 0 && !restOfTheData->hasRemaining())) {
                restOfTheData->reuse();
                restOfTheData = nullptr;
            } else {
                restOfTheData->compact();
                restOfTheData->limit(restOfTheData->position());
                restOfTheData->position(0);
            }
        }

        if (parseLaterBuffer != nullptr) {
            buffer = parseLaterBuffer;
            parseLaterBuffer = nullptr;
        }
    }
}

void Connection::connect() {
    if (waitForReconnectTimer) {
        return;
    }
    if (!ConnectionsManager::getInstance(currentDatacenter->instanceNum).isNetworkAvailable()) {
        ConnectionsManager::getInstance(currentDatacenter->instanceNum).onConnectionClosed(this, 0);
        return;
    }
    if (connectionState == TcpConnectionStageConnected || connectionState == TcpConnectionStageConnecting) {
        return;
    }
    connectionInProcess = true;
    connectionState = TcpConnectionStageConnecting;
    isMediaConnection = false;
    uint32_t ipv6 = ConnectionsManager::getInstance(currentDatacenter->instanceNum).isIpv6Enabled() ? TcpAddressFlagIpv6 : 0;
    uint32_t isStatic = connectionType == ConnectionTypeProxy || !ConnectionsManager::getInstance(currentDatacenter->instanceNum).proxyAddress.empty() ? TcpAddressFlagStatic : 0;
    TcpAddress *tcpAddress = nullptr;
    if (isMediaConnectionType(connectionType)) {
        currentAddressFlags = TcpAddressFlagDownload | isStatic;
        tcpAddress = currentDatacenter->getCurrentAddress(currentAddressFlags | ipv6);
        if (tcpAddress == nullptr) {
            currentAddressFlags = isStatic;
            tcpAddress = currentDatacenter->getCurrentAddress(currentAddressFlags | ipv6);
        } else {
            isMediaConnection = true;
        }
        if (tcpAddress == nullptr && ipv6) {
            ipv6 = 0;
            currentAddressFlags = TcpAddressFlagDownload | isStatic;
            tcpAddress = currentDatacenter->getCurrentAddress(currentAddressFlags);
            if (tcpAddress == nullptr) {
                currentAddressFlags = isStatic;
                tcpAddress = currentDatacenter->getCurrentAddress(currentAddressFlags);
            } else {
                isMediaConnection = true;
            }
        }
    } else if (connectionType == ConnectionTypeTemp) {
        currentAddressFlags = TcpAddressFlagTemp;
        tcpAddress = currentDatacenter->getCurrentAddress(currentAddressFlags);
    } else {
        currentAddressFlags = isStatic;
        tcpAddress = currentDatacenter->getCurrentAddress(currentAddressFlags | ipv6);
        if (tcpAddress == nullptr && ipv6) {
            ipv6 = 0;
            tcpAddress = currentDatacenter->getCurrentAddress(currentAddressFlags);
        }
    }
    if (tcpAddress == nullptr) {
        hostAddress = "";
    } else {
        hostAddress = tcpAddress->address;
        secret = tcpAddress->secret;
    }
    if (tcpAddress != nullptr && isStatic) {
        hostPort = (uint16_t) tcpAddress->port;
    } else {
        hostPort = (uint16_t) currentDatacenter->getCurrentPort(currentAddressFlags);
    }

    reconnectTimer->stop();

    if (LOGS_ENABLED) DEBUG_D("connection(%p, account%u, dc%u, type %d) connecting (%s:%hu)", this, currentDatacenter->instanceNum, currentDatacenter->getDatacenterId(), connectionType, hostAddress.c_str(), hostPort);
    firstPacketSent = false;
    if (restOfTheData != nullptr) {
        restOfTheData->reuse();
        restOfTheData = nullptr;
    }
    lastPacketLength = 0;
    wasConnected = false;
    hasSomeDataSinceLastConnect = false;
    openConnection(hostAddress, hostPort, ipv6 != 0, ConnectionsManager::getInstance(currentDatacenter->instanceNum).currentNetworkType);
    if (connectionType == ConnectionTypeProxy) {
        setTimeout(5);
    } else if (connectionType == ConnectionTypePush) {
        if (isTryingNextPort) {
            setTimeout(20);
        } else {
            setTimeout(30);
        }
    } else if (connectionType == ConnectionTypeUpload) {
        if (ConnectionsManager::getInstance(currentDatacenter->instanceNum).networkSlow) {
            setTimeout(40);
        } else {
            setTimeout(25);
        }
    } else {
        if (isTryingNextPort) {
            setTimeout(8);
        } else {
            setTimeout(12);
        }
    }
    connectionInProcess = false;
}

void Connection::reconnect() {
    if (connectionType == ConnectionTypeProxy) {
        suspendConnection(false);
    } else {
        forceNextPort = true;
        suspendConnection(true);
        connect();
    }
}

bool Connection::hasUsefullData() {
    int64_t time = ConnectionsManager::getInstance(currentDatacenter->instanceNum).getCurrentTimeMonotonicMillis();
    if (usefullData && llabs(time - usefullDataReceiveTime) < 4 * 1000L) {
        return false;
    }
    return usefullData;
}

bool Connection::isSuspended() {
    return connectionState == TcpConnectionStageSuspended;
}

bool Connection::isMediaConnectionType(ConnectionType type) {
    return (type & ConnectionTypeGenericMedia) != 0 || (type & ConnectionTypeDownload) != 0;
}

void Connection::setHasUsefullData() {
    if (!usefullData) {
        usefullDataReceiveTime = ConnectionsManager::getInstance(currentDatacenter->instanceNum).getCurrentTimeMonotonicMillis();
        usefullData = true;
        lastReconnectTimeout = 50;
    }
}

bool Connection::allowsCustomPadding() {
    return currentProtocolType == ProtocolTypeDD || currentProtocolType == ProtocolTypeEF;
}

void Connection::sendData(NativeByteBuffer *buff, bool reportAck, bool encrypted) {
    if (buff == nullptr) {
        return;
    }
    buff->rewind();
    if (connectionState == TcpConnectionStageIdle || connectionState == TcpConnectionStageReconnecting || connectionState == TcpConnectionStageSuspended) {
        connect();
    }

    if (isDisconnected()) {
        buff->reuse();
        if (LOGS_ENABLED) DEBUG_D("connection(%p, account%u, dc%u, type %d) disconnected, don't send data", this, currentDatacenter->instanceNum, currentDatacenter->getDatacenterId(), connectionType);
        return;
    }

    //TODO must delete
    uint32_t packetLength = buff->limit();
    NativeByteBuffer *buffer = BuffersStorage::getInstance().getFreeBuffer(8);
    if (!firstPacketSent)
        firstPacketSent = true;
    buffer->writeInt32(packetLength + 4);//len
    buffer->writeInt32(0);//seq
    buffer->rewind();
    writeBuffer(buffer);
    buff->rewind();
    writeBuffer(buff);
}

inline char char2int(char input) {
    if (input >= '0' && input <= '9') {
        return input - '0';
    } else if (input >= 'A' && input <= 'F') {
        return (char) (input - 'A' + 10);
    } else if (input >= 'a' && input <= 'f') {
        return (char) (input - 'a' + 10);
    }
    return 0;
}

inline std::string *Connection::getCurrentSecret(uint8_t secretType) {
    if (secretType == 2) {
        return &secret;
    } else if (!overrideProxySecret.empty()) {
        return &overrideProxySecret;
    } else {
        return &ConnectionsManager::getInstance(currentDatacenter->instanceNum).proxySecret;
    }
}

inline void Connection::encryptKeyWithSecret(uint8_t *bytes, uint8_t secretType) {
    if (secretType == 0) {
        return;
    }
    std::string *currentSecret = getCurrentSecret(secretType);
    size_t a = 0;
    if (currentSecret->length() == 34 && (*currentSecret)[0] == 'd' && (*currentSecret)[1] == 'd') {
        a = 1;
    }

    SHA256_CTX sha256Ctx;
    SHA256_Init(&sha256Ctx);
    SHA256_Update(&sha256Ctx, bytes, 32);
    char b[1];
    for (; a < currentSecret->size() / 2; a++) {
        b[0] = (char) (char2int((*currentSecret)[a * 2]) * 16 + char2int((*currentSecret)[a * 2 + 1]));
        SHA256_Update(&sha256Ctx, b, 1);
    }
    SHA256_Final(bytes, &sha256Ctx);
}

void Connection::onDisconnectedInternal(int32_t reason, int32_t error) {
    reconnectTimer->stop();
    if (LOGS_ENABLED) DEBUG_D("connection(%p, account%u, dc%u, type %d) disconnected with reason %d", this, currentDatacenter->instanceNum, currentDatacenter->getDatacenterId(), connectionType, reason);
    bool switchToNextPort = reason == 2 && wasConnected && (!hasSomeDataSinceLastConnect || currentDatacenter->isCustomPort(currentAddressFlags)) || forceNextPort;
    if (connectionType == ConnectionTypeGeneric || connectionType == ConnectionTypeTemp || connectionType == ConnectionTypeGenericMedia) {
        if (wasConnected && reason == 2 && currentTimeout < 16) {
            currentTimeout += 2;
        }
    }
    firstPacketSent = false;
    if (restOfTheData != nullptr) {
        restOfTheData->reuse();
        restOfTheData = nullptr;
    }
    lastPacketLength = 0;
    receivedDataAmount = 0;
    wasConnected = false;
    if (connectionState != TcpConnectionStageSuspended && connectionState != TcpConnectionStageIdle) {
        connectionState = TcpConnectionStageIdle;
    }
    ConnectionsManager::getInstance(currentDatacenter->instanceNum).onConnectionClosed(this, reason);
    connectionToken = 0;

    uint32_t datacenterId = currentDatacenter->getDatacenterId();
    if (connectionState == TcpConnectionStageIdle) {
        connectionState = TcpConnectionStageReconnecting;
        failedConnectionCount++;
        if (failedConnectionCount == 1) {
            if (hasUsefullData()) {
                willRetryConnectCount = 3;
            } else {
                willRetryConnectCount = 1;
            }
        }
        if (ConnectionsManager::getInstance(currentDatacenter->instanceNum).isNetworkAvailable() && connectionType != ConnectionTypeProxy) {
            isTryingNextPort = true;
            if (failedConnectionCount > willRetryConnectCount || switchToNextPort) {
                currentDatacenter->nextAddressOrPort(currentAddressFlags);
                failedConnectionCount = 0;
            }
        }
        if (error == 0x68 || error == 0x71) {
            if (connectionType != ConnectionTypeProxy) {
                waitForReconnectTimer = true;
                reconnectTimer->setTimeout(lastReconnectTimeout, false);
                lastReconnectTimeout *= 2;
                if (lastReconnectTimeout > 400) {
                    lastReconnectTimeout = 400;
                }
                reconnectTimer->start();
            }
        } else {
            waitForReconnectTimer = false;
            if (connectionType == ConnectionTypeGenericMedia && currentDatacenter->isHandshaking(true) || connectionType == ConnectionTypeGeneric && (currentDatacenter->isHandshaking(false) || datacenterId == ConnectionsManager::getInstance(currentDatacenter->instanceNum).currentDatacenterId || datacenterId == ConnectionsManager::getInstance(currentDatacenter->instanceNum).movingToDatacenterId)) {
                if (LOGS_ENABLED) DEBUG_D("connection(%p, account%u, dc%u, type %d) reconnect %s:%hu", this, currentDatacenter->instanceNum, currentDatacenter->getDatacenterId(), connectionType, hostAddress.c_str(), hostPort);
                reconnectTimer->setTimeout(1000, false);
                reconnectTimer->start();
            }
        }
    }
    usefullData = false;
}

void Connection::onDisconnected(int32_t reason, int32_t error) {
    if (connectionInProcess) {
        ConnectionsManager::getInstance(currentDatacenter->instanceNum).scheduleTask([&, reason, error] {
            onDisconnectedInternal(reason, error);
        });
    } else {
        onDisconnectedInternal(reason, error);
    }
}

void Connection::onConnected() {
    connectionState = TcpConnectionStageConnected;
    connectionToken = lastConnectionToken++;
    wasConnected = true;
    if (LOGS_ENABLED) DEBUG_D("connection(%p, account%u, dc%u, type %d) connected to %s:%hu", this, currentDatacenter->instanceNum, currentDatacenter->getDatacenterId(), connectionType, hostAddress.c_str(), hostPort);
    ConnectionsManager::getInstance(currentDatacenter->instanceNum).onConnectionConnected(this);
}

bool Connection::hasPendingRequests() {
    return ConnectionsManager::getInstance(currentDatacenter->instanceNum).hasPendingRequestsForConnection(this);
}

Datacenter *Connection::getDatacenter() {
    return currentDatacenter;
}

ConnectionType Connection::getConnectionType() {
    return connectionType;
}

int8_t Connection::getConnectionNum() {
    return connectionNum;
}

uint32_t Connection::getConnectionToken() {
    return connectionToken;
}

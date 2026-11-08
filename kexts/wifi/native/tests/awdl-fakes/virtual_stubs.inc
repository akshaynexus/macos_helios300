void IO80211VirtualInterface::setLinkState(IO80211LinkState a0, unsigned int a1){}
IOReturn IO80211VirtualInterface::forwardPacket(IO80211NetworkPacket* a0){return 0;}
IOReturn IO80211VirtualInterface::handleIoctl(__ifnet* a0, unsigned long a1, void* a2){return 0;}
void IO80211VirtualInterface::addPeerToCache(unsigned char* a0){}
void IO80211VirtualInterface::deletePeerFromCache(unsigned char* a0){}
IOReturn IO80211VirtualInterface::handleDebugCmd(apple80211_debug_command* a0){return 0;}
IOReturn IO80211VirtualInterface::getPeerSlotDataStats(ether_addr* a0, bool a1, peerSlotDataStats* a2){return 0;}
IOReturn IO80211VirtualInterface::getPeerDataStats(ether_addr* a0, bool a1, packetCounters_t* a2){return 0;}
void IO80211VirtualInterface::clearPeerDataStats(ether_addr* a0, bool a1){}
void IO80211VirtualInterface::setAwdlCurrentChannelSequenceIndex(unsigned int a0){}
bool IO80211VirtualInterface::isOutputFlowControlled(){return false;}
void IO80211VirtualInterface::setOutputFlowControlled(){}
void IO80211VirtualInterface::clearOutputFlowControlled(){}
void IO80211VirtualInterface::flushFlowQueues(ether_addr* a0){}
IOReturn IO80211VirtualInterface::configureAQMOutput(){return 0;}
bool IO80211VirtualInterface::attachToBpf(){return false;}
bool IO80211VirtualInterface::configureIfnet(){return false;}
void IO80211VirtualInterface::reset(){}
IOReturn IO80211VirtualInterface::dupAndTransmitMcastPacket(IO80211NetworkPacket* a0, unsigned char* a1, unsigned char* a2){return 0;}
void IO80211VirtualInterface::freeMulticastPacket(IO80211NetworkPacket* a0){}
IOReturn IO80211VirtualInterface::triggerAWDLMulticastTx(unsigned int a0, unsigned char a1){return 0;}
IOReturn IO80211VirtualInterface::p2pMulticastTx(IO80211NetworkPacket** a0, unsigned int a1){return 0;}
IOReturn IO80211VirtualInterface::p2pMulticastTx(unsigned int a0, bool a1){return 0;}
void IO80211VirtualInterface::signalMulticastPackets(unsigned int a0, unsigned int a1){}
IOReturn IO80211VirtualInterface::sendPendingPacketsToStack(IO80211NetworkPacket** a0, unsigned int a1){return 0;}
void IO80211VirtualInterface::freePendingPackets(IO80211NetworkPacket** a0, unsigned int a1){}

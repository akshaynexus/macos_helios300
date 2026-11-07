#pragma once
#include <IOKit/IOUserClient.h>
class Ath10kPCIDevice;
class Ath10kUserClient:public IOUserClient {
 OSDeclareDefaultStructors(Ath10kUserClient)
 Ath10kPCIDevice *device=nullptr;
 task_t openingTask=nullptr;
public:
 bool initWithTask(task_t,void *,UInt32,OSDictionary *) override;
 void free() override;
 bool start(IOService *) override;
 void stop(IOService *) override;
 IOReturn clientClose() override;
 IOReturn externalMethod(uint32_t,IOExternalMethodArguments *,IOExternalMethodDispatch *,OSObject *,void *) override;
};

#!/usr/bin/env python3
"""Compile actual authorization and dispatch methods with task/IOKit test doubles."""
from pathlib import Path
import subprocess, tempfile
root=Path(__file__).resolve().parents[1]
s=(root/'src/kext/Ath10kUserClient.cpp').read_text()
policy=s[s.index('static bool athControlAllowed'):s.index('bool Ath10kUserClient::start')]
dispatch=s[s.index('IOReturn Ath10kUserClient::externalMethod'):s.index('IOReturn Ath10kPCIDevice::newUserClient')]
stub=r'''
#include <cassert>
#include <cstdint>
#include <cstddef>
using task_t=void*; using UInt32=uint32_t; using IOReturn=int;
struct OSDictionary{}; struct OSObject{};struct IOExternalMethodDispatch{};
constexpr int kIOReturnSuccess=0,kIOReturnNotAttached=1,kIOReturnBadArgument=2,kIOReturnNotPrivileged=3;
const char *kIOClientPrivilegeLocalUser="local",*kIOClientPrivilegeAdministrator="root";
void *caller=nullptr;bool local=false,admin=false,baseInit=true;int refs=0,freed=0;
task_t current_task(){return caller;}void task_reference(task_t){refs++;}void task_deallocate(task_t){refs--;}
namespace AthControl{struct Request{char data[528];};struct Response{char data[644];};}
struct IOExternalMethodArguments{
 size_t structureInputSize,structureOutputSize;void *structureInput,*structureOutput;
 unsigned scalarInputCount=0,scalarOutputCount=0;void *structureInputDescriptor=nullptr,*structureOutputDescriptor=nullptr;
};
struct IOUserClient{
 static int clientHasPrivilege(task_t,const char *p){return (p==kIOClientPrivilegeLocalUser?local:admin)?0:3;}
 bool initWithTask(task_t,void*,UInt32,OSDictionary*){return baseInit;}
 void free(){freed++;}
};
struct Device{int calls=0;int controlRequest(void*,const AthControl::Request*,AthControl::Response*){calls++;return 0;}};
struct Ath10kUserClient:IOUserClient{
 task_t openingTask=nullptr;Device *device=nullptr;bool inactive=false;
 bool isInactive(){return inactive;}
 bool initWithTask(task_t,void*,UInt32,OSDictionary*);void free();
 int externalMethod(uint32_t,IOExternalMethodArguments*,IOExternalMethodDispatch*,OSObject*,void*);
};
#define super IOUserClient
'''
main=r'''
int main(){
 int a,b;caller=&a;Ath10kUserClient c;Device d;c.device=&d;
 assert(!c.initWithTask(caller,nullptr,0,nullptr));assert(refs==0);
 local=true;assert(c.initWithTask(caller,nullptr,0,nullptr));assert(refs==1);
 AthControl::Request req{};AthControl::Response out{};
 IOExternalMethodArguments args{sizeof(req),sizeof(out),&req,&out};
 auto call=[&](){return c.externalMethod(0,&args,nullptr,nullptr,nullptr);};
 assert(call()==0 && d.calls==1);
 caller=&b;assert(call()==kIOReturnNotPrivileged && d.calls==1);
 caller=&a;local=false;assert(call()==kIOReturnNotPrivileged);
 admin=true;assert(call()==0 && d.calls==2);
 args.structureInputSize--;assert(call()==kIOReturnBadArgument);args.structureInputSize++;
 args.scalarInputCount=1;assert(call()==kIOReturnBadArgument);args.scalarInputCount=0;
 c.inactive=true;assert(call()==kIOReturnNotAttached);
 c.free();assert(refs==0 && freed==1);
 Ath10kUserClient failed;baseInit=false;assert(!failed.initWithTask(caller,nullptr,0,nullptr));
 failed.free();assert(refs==0);
 assert(!athControlAllowed(nullptr));
}
'''
with tempfile.TemporaryDirectory() as tmp:
 p=Path(tmp)/'access.cpp';p.write_text(stub+policy+dispatch+main)
 subprocess.run(['c++','-std=c++17','-fsanitize=address,undefined','-fno-omit-frame-pointer',str(p),'-o',tmp+'/access'],check=True)
 subprocess.run([tmp+'/access'],check=True)
print('UserClient access: local/root, revoked access, transferred port, malformed input, task lifetime passed')

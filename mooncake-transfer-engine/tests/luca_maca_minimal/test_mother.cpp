// Real port and old-framework sources; SDK/metadata are CPU dependency doubles.
// Six independent processes cover 3 copy choices x 2 caller-sync choices.
#include "multi_transport.h"
#include "transfer_engine.h"
#include "transport/luca_transport/luca_transport.h"
#include "mock_luca.h"
#include "config.h"
#include <algorithm>
#include <cassert>
#include <cstdlib>
#include <cstring>
#include <iostream>
using namespace mooncake;
using Request=Transport::TransferRequest;
void env(const char* name,const char* value) {
#ifdef _WIN32
    _putenv_s(name,value);
#else
    setenv(name,value,1);
#endif
}
int main(int argc,char**argv) {
    assert(argc==3);
    std::string mode=argv[1],sync=argv[2];
    env("MC_LUCA_COPY_API",mode.c_str());env("MC_LUCA_CALLER_SYNC",sync.c_str());
    env("MC_LUCA_BATCHFLAG_MIN_BYTES","64");env("MC_LUCA_HOST_ID","host-double");
    globalConfig().slice_timeout=0;
    auto* src=static_cast<unsigned char*>(mock::allocate(16384));
    auto* dst=static_cast<unsigned char*>(mock::allocate(16384));
    for(size_t i=0;i<16384;++i){src[i]=static_cast<unsigned char>(i*11);dst[i]=static_cast<unsigned char>(i*7+9);}
    auto meta=std::make_shared<TransferMetadata>();std::string name="local";
    auto engine=std::unique_ptr<MultiTransport>(new MultiTransport(meta,name));
    auto* transport=engine->installTransport("luca",std::make_shared<Topology>());assert(transport);
    assert(TransferEngine::registerBuffer(*transport,dst+4096,256)==0);
    assert(TransferEngine::registerBuffer(*transport,dst+8192,256)==0);
    assert(meta->segments[0]->buffers.size()==2);
    meta->segments[1]=std::make_shared<TransferMetadata::SegmentDesc>(*meta->segments[0]);
    auto request=[&](Request::OpCode op,size_t local,size_t peer,size_t n) {
        return Request{op,src+local,1,reinterpret_cast<uint64_t>(dst+peer),n};
    };
    std::vector<unsigned char> expected_src(src,src+16384),expected_dst(dst,dst+16384);
    std::copy(dst+4096,dst+4128,expected_src.begin()+128);
    std::copy(src+256,src+384,expected_dst.begin()+4160);
    std::copy(src+512,src+544,expected_dst.begin()+8240);
    auto id=engine->allocateBatchID(3);
    {std::vector<Request> req{request(Request::READ,128,4096,32),request(Request::WRITE,256,4160,128),request(Request::WRITE,512,8240,32)};
     assert(engine->submitTransfer(id,req).ok());}
    Transport::TransferStatus status{};
    assert(engine->getBatchTransferStatus(id,status).ok()&&status.s==Transport::WAITING);
    assert(!engine->freeBatchID(id).ok());
    assert(mock::state.syncs==0); // mother caller event, no device-wide barrier
    assert(mock::state.caller_syncs==(sync=="host"?1:0));
    assert(mock::state.stream_waits==(sync=="wait"?1:0));
    assert(mock::state.copies==(mode=="default"?3:mode=="auto"?2:0));
    assert(mock::state.batches==(mode=="default"?0:1));
    mock::completeAll();
    assert(engine->getBatchTransferStatus(id,status).ok()&&status.s==Transport::COMPLETED&&status.transferred_bytes==192);
    assert(std::equal(expected_src.begin(),expected_src.end(),src));
    assert(std::equal(expected_dst.begin(),expected_dst.end(),dst));
    assert(engine->freeBatchID(id).ok());
    assert(mock::state.opens==1&&mock::state.closes==0);
    // Direct backend entry retains mother's synchronous behavior, even in batchflag mode.
    auto direct=transport->allocateBatchID(1);
    std::vector<Request> direct_req{request(Request::WRITE,1024,4304,16)};
    assert(transport->submitTransfer(direct,direct_req).ok());
    assert(mock::state.direct_copies==1&&std::equal(src+1024,src+1040,dst+4304));
    assert(transport->getTransferStatus(direct,0,status).ok()&&status.s==Transport::COMPLETED);
    assert(transport->freeBatchID(direct).ok());
    assert(mock::state.opens==1); // persistent cache reused across submissions
    // Allocation-valid but outside the registered logical range is rejected.
    auto bad=engine->allocateBatchID(1);std::vector<Request> bad_req{request(Request::WRITE,0,5000,16)};
    assert(engine->submitTransfer(bad,bad_req).ok()==false); // mother's first_error contract
    assert(engine->getBatchTransferStatus(bad,status).ok()&&status.s==Transport::FAILED);
    assert(engine->freeBatchID(bad).ok());
    // Mother's stream-level polling waits for B even after A's own copy finished.
    auto a=engine->allocateBatchID(1),b=engine->allocateBatchID(1);
    std::vector<Request> ar{request(Request::WRITE,0,4096,8)},br{request(Request::WRITE,8,4104,8)};
    assert(engine->submitTransfer(a,ar).ok());assert(engine->submitTransfer(b,br).ok());
    mock::completeOne();
    assert(engine->getBatchTransferStatus(a,status).ok()&&status.s==Transport::WAITING);
    mock::completeAll();
    assert(engine->getBatchTransferStatus(a,status).ok()&&status.s==Transport::COMPLETED);
    assert(engine->getBatchTransferStatus(b,status).ok()&&status.s==Transport::COMPLETED);
    assert(engine->freeBatchID(a).ok());assert(engine->freeBatchID(b).ok());
    engine.reset();assert(mock::state.opens==1&&mock::state.closes==1&&mock::state.unsafe_closes==0);
    mock::release(src);mock::release(dst);
    std::cout<<"MACA_MINIMAL_CPU_PASS="<<mode<<"/"<<sync<<"\n";
}

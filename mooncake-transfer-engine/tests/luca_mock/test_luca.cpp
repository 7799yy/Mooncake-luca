// The transport, MultiTransport, slice lifecycle and Status under test are the
// production sources. Only the SDK, metadata storage and external deps are
// fake.
#include "multi_transport.h"
#include "transfer_engine.h"
#include "transport/luca_transport/luca_transport.h"
#include "mock_luca.h"
#include "config.h"
#include <algorithm>
#include <cassert>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <iostream>
#include <limits>
#include <string>

using namespace mooncake;
using Request = Transport::TransferRequest;
void env(const char* name, const char* value) {
#ifdef _WIN32
    _putenv_s(name, value ? value : "");
#else
    if (value)
        setenv(name, value, 1);
    else
        unsetenv(name);
#endif
}
struct Buffer {
    size_t size;
    unsigned char* data;
    explicit Buffer(size_t n)
        : size(n), data(static_cast<unsigned char*>(mock::allocate(n))) {}
    ~Buffer() { mock::release(data); }
    std::vector<unsigned char> bytes() const { return {data, data + size}; }
};
struct Harness {
    Buffer source{16384}, remote{16384};
    std::string name = "local";
    std::shared_ptr<TransferMetadata> metadata =
        std::make_shared<TransferMetadata>();
    std::unique_ptr<MultiTransport> engine;
    Transport* luca;
    std::vector<Transport::BatchID> batches;
    explicit Harness(const char* mode = "default",
                     const char* threshold = nullptr) {
        env("MC_LUCA_HOST_ID", "mock-host");
        env("MC_LUCA_COPY_API", mode);
        env("MC_LUCA_BATCHFLAG_MIN_BYTES", threshold);
        globalConfig().slice_timeout = 0;
        std::memset(source.data, 0x31, source.size);
        std::memset(remote.data, 0xa5, remote.size);
        for (size_t i = 0; i < 256; ++i)
            remote.data[4096 + i] = static_cast<unsigned char>(i + 7);
        engine.reset(new MultiTransport(metadata, name));
        luca = engine->installTransport("luca", std::make_shared<Topology>());
        assert(luca);
    }
    void publish(size_t offset = 4096, size_t length = 256) {
        assert(TransferEngine::registerBuffer(*luca, remote.data + offset,
                                              length) == 0);
        metadata->segments[1] = std::make_shared<TransferMetadata::SegmentDesc>(
            *metadata->segments.at(0));
        metadata->segments[1]->name = "remote";
    }
    Request request(Request::OpCode op, size_t local, size_t peer,
                    size_t length) {
        return Request{op, source.data + local, 1,
                       reinterpret_cast<uint64_t>(remote.data + peer), length};
    }
    Transport::BatchID submit(std::vector<Request> requests) {
        auto id = engine->allocateBatchID(requests.size());
        assert(engine->submitTransfer(id, requests).ok());
        batches.push_back(id);
        // The request vector intentionally expires here: polling must use the
        // resolved task/slice state, not a dangling TransferTask::request.
        return id;
    }
    Transport::TransferStatus status(Transport::BatchID id) {
        Transport::TransferStatus value{};
        assert(engine->getBatchTransferStatus(id, value).ok());
        return value;
    }
    void free(Transport::BatchID id) {
        assert(engine->freeBatchID(id).ok());
        batches.erase(std::find(batches.begin(), batches.end(), id));
    }
    ~Harness() {
        mock::state.fail_pending_drain = 0;
        mock::state.fail_query = 0;
        mock::completeAll();
        for (auto id : batches) {
            auto value = status(id);
            assert(value.s == Transport::COMPLETED ||
                   value.s == Transport::FAILED);
            assert(engine->freeBatchID(id).ok());
        }
        engine.reset();
        assert(mock::state.unsafe_closes == 0);
    }
};

void correctCopies(const char* mode) {
    Harness h(mode);
    h.publish();
    auto src = h.source.bytes(), dst = h.remote.bytes();
    auto id = h.submit({h.request(Request::READ, 128, 4096, 64),
                        h.request(Request::WRITE, 0, 4224, 64)});
    assert(h.status(id).s == Transport::WAITING);
    assert(!h.engine->freeBatchID(id).ok());
    assert(h.source.bytes() == src && h.remote.bytes() == dst);
    assert(mock::state.opens == 1 && mock::state.closes == 0);
    assert(mock::state.syncs == 1);  // one producer barrier, not two per task
    std::copy(dst.begin() + 4096, dst.begin() + 4160, src.begin() + 128);
    std::copy(h.source.data, h.source.data + 64, dst.begin() + 4224);
    mock::completeAll();
    auto result = h.status(id);
    assert(result.s == Transport::COMPLETED && result.transferred_bytes == 128);
    assert(h.source.bytes() == src && h.remote.bytes() == dst);
    assert(mock::state.closes == 1);
    assert(h.status(id).transferred_bytes == 128);  // no double markSuccess
    if (std::string(mode) == "batchflag")
        assert(mock::state.batches == 1 && mock::state.copies == 0);
    else
        assert(mock::state.copies == 2 && mock::state.batches == 0);
    h.free(id);
}
void partialFailureWaits() {
    Harness h;
    h.publish();
    auto id = h.submit({h.request(Request::WRITE, 0, 4096, 32),
                        h.request(Request::WRITE, 64, 4350, 16)});
    assert(h.status(id).s ==
           Transport::WAITING);  // failed logical range + active copy
    assert(!h.engine->freeBatchID(id).ok());
    mock::completeAll();
    assert(h.status(id).s == Transport::FAILED);
    h.free(id);
}
void overlappingBatches() {
    Harness h;
    h.publish();
    auto a = h.submit({h.request(Request::WRITE, 0, 4096, 32)});
    auto b = h.submit({h.request(Request::WRITE, 32, 4128, 32)});
    // Submit B's producer barrier completed A, but B is still queued. A's
    // event must not wait for the shared stream to become globally idle.
    assert(h.status(a).s == Transport::COMPLETED);
    assert(h.status(b).s == Transport::WAITING);
    assert(mock::state.opens == 1 && mock::state.closes == 0);
    h.free(a);
    mock::completeAll();
    assert(h.status(b).s == Transport::COMPLETED);
    assert(mock::state.closes == 1);
    h.free(b);
}
void badEnvelope(const std::string& field) {
    Harness h;
    h.publish();
    auto& buffer = h.metadata->segments[1]->buffers[0];
    Json::Value value;
    Json::Reader reader;
    assert(reader.parse(buffer.shm_name, value, false));
    if (field == "host_id")
        value[field] = "another-host";
    else if (field == "abi_size")
        value[field] = Json::UInt64(80);
    else if (field == "allocation_size")
        value[field] = Json::UInt64(16);
    else if (field == "handle_hex")
        value[field] = "zz";
    else
        value["format"] = "wrong";
    buffer.shm_name = value.toStyledString();
    auto id = h.submit({h.request(Request::WRITE, 0, 4096, 16)});
    assert(h.status(id).s == Transport::FAILED);
    assert(mock::state.opens == 0 && mock::state.copies == 0);
    h.free(id);
}
void enqueueFailure(bool batch, bool drainFails) {
    Harness h(batch ? "batchflag" : "default");
    h.publish();
    if (batch)
        mock::state.fail_batch_at = 1;
    else
        mock::state.fail_copy_at = 2;
    if (drainFails) mock::state.fail_pending_drain = 10;
    auto id = h.submit({h.request(Request::WRITE, 0, 4096, 32),
                        h.request(Request::WRITE, 32, 4128, 32)});
    if (drainFails) {
        assert(mock::state.closes == 0 && mock::pendingCopies() > 0);
        assert(h.status(id).s == Transport::WAITING);
        assert(!h.engine->freeBatchID(id).ok());
        mock::state.fail_pending_drain = 0;
    }
    assert(h.status(id).s == Transport::FAILED);
    assert(mock::pendingCopies() == 0 && mock::state.closes == 1);
    h.free(id);
}
void eventFailure(bool record) {
    Harness h;
    h.publish();
    if (record) mock::state.fail_record = 1;
    auto id = h.submit({h.request(Request::WRITE, 0, 4096, 64)});
    if (!record) mock::state.fail_query = 1;
    assert(h.status(id).s == Transport::FAILED);
    assert(mock::pendingCopies() == 0 && mock::state.closes == 1);
    assert(mock::state.event_destroys == 1);
    h.free(id);
}
void registrationTransactions() {
    Harness h;
    auto* a = h.remote.data + 4096;
    auto* b = h.remote.data + 8192;
    assert(TransferEngine::registerBuffer(*h.luca, a, 64) == 0);
    assert(TransferEngine::registerBuffer(*h.luca, b, 64) == 0);
    assert(mock::state.exports == 1);  // one handle per true allocation
    assert(TransferEngine::registerBatch(*h.luca,
                                         {{h.source.data, 64}, {a, 64}}) != 0);
    assert(h.metadata->segments[0]->buffers.size() == 2);
    h.metadata->publication_failures = 1;
    assert(TransferEngine::registerBuffer(*h.luca, h.source.data, 64) != 0);
    assert(h.metadata->segments[0]->buffers.size() == 2);
    h.metadata->publication_failures = 1;
    assert(TransferEngine::unregisterBuffer(*h.luca, a) != 0);
    assert(h.metadata->segments[0]->buffers.size() == 2);
    assert(TransferEngine::unregisterBuffer(*h.luca, a) == 0);
    assert(TransferEngine::unregisterBuffer(*h.luca, b) == 0);
    assert(h.metadata->segments[0]->buffers.empty());
}
void unregisterDrains() {
    Harness h;
    h.publish();
    assert(TransferEngine::registerBuffer(*h.luca, h.source.data, 64) == 0);
    auto id = h.submit({h.request(Request::WRITE, 0, 4096, 64)});
    assert(TransferEngine::unregisterBuffer(*h.luca, h.source.data) == 0);
    assert(mock::pendingCopies() == 0 &&
           h.status(id).s == Transport::COMPLETED);
    h.free(id);
}
void mixedAutoOrdering() {
    Harness h("auto", "32");
    h.publish();
    // A batchflag write followed by a small async write to the same location.
    // Collecting all large copies until the end would reverse the result.
    std::memset(h.source.data, 0x11, 64);
    std::memset(h.source.data + 128, 0x77, 16);
    auto id = h.submit({h.request(Request::WRITE, 0, 4096, 64),
                        h.request(Request::WRITE, 128, 4096, 16)});
    mock::completeAll();
    assert(h.status(id).s == Transport::COMPLETED);
    assert(std::all_of(h.remote.data + 4096, h.remote.data + 4112,
                       [](unsigned char v) { return v == 0x77; }));
    assert(mock::state.copies == 1 && mock::state.batches == 1);
    h.free(id);
}
void timeoutDrains() {
    Harness h;
    h.publish();
    globalConfig().slice_timeout = 1;
    auto id = h.submit({h.request(Request::WRITE, 0, 4096, 64)});
    mock_now_ns += 2000000000LL;
    assert(h.status(id).s == Transport::FAILED);
    assert(mock::pendingCopies() == 0);
    h.free(id);
}
void rejectedRouteRollsBack() {
    Harness h;
    h.publish();
    auto id = h.engine->allocateBatchID(2);
    auto bad = h.request(Request::WRITE, 0, 4096, 16);
    bad.target_id = 999;
    std::vector<Request> entries{h.request(Request::WRITE, 0, 4096, 16), bad};
    assert(!h.engine->submitTransfer(id, entries).ok());
    assert(reinterpret_cast<Transport::BatchDesc*>(id)->task_list.empty());
    assert(h.engine->freeBatchID(id).ok());
}
void counterBackendUnchanged() {
    Harness h;
    auto peer = std::make_shared<TransferMetadata::SegmentDesc>();
    peer->protocol = "rdma";
    h.metadata->segments[1] = peer;
    assert(h.engine->installTransport("rdma", std::make_shared<Topology>()));
    auto id = h.submit({h.request(Request::WRITE, 0, 4096, 8)});
    assert(h.status(id).s == Transport::COMPLETED);
    h.free(id);
}
void zeroAndLocal() {
    Harness h;
    auto id = h.engine->allocateBatchID(2);
    std::vector<Request> requests{
        {Request::WRITE, nullptr, 0, 0, 0},
        {Request::WRITE, h.source.data, 0,
         reinterpret_cast<uint64_t>(h.remote.data), 16}};
    assert(h.engine->submitTransfer(id, requests).ok());
    h.batches.push_back(id);
    assert(h.status(id).s == Transport::WAITING);
    mock::completeAll();
    assert(h.status(id).s == Transport::COMPLETED);
    assert(std::memcmp(h.source.data, h.remote.data, 16) == 0);
    h.free(id);
}
void closeFailureRetained() {
    Harness h;
    h.publish();
    auto id = h.submit({h.request(Request::WRITE, 0, 4096, 16)});
    mock::state.fail_close = 1;
    mock::completeAll();
    assert(h.status(id).s == Transport::FAILED);
    assert(mock::state.closes == 1);
    h.free(id);
}
void directSubmission() {
    Harness h;
    h.publish();
    auto id = h.luca->allocateBatchID(1);
    std::vector<Request> requests{h.request(Request::WRITE, 0, 4096, 16)};
    assert(h.luca->submitTransfer(id, requests).ok());
    assert(h.luca->submitTransfer(id, requests).IsTooManyRequests());
    Transport::TransferStatus value{};
    assert(h.luca->getTransferStatus(id, 0, value).ok() &&
           value.s == Transport::WAITING);
    assert(h.luca->getTransferStatus(id, 1, value).IsInvalidArgument());
    mock::completeAll();
    assert(h.luca->getTransferStatus(id, 0, value).ok() &&
           value.s == Transport::COMPLETED);
    assert(h.luca->freeBatchID(id).ok());
}
void invalidRequests() {
    Harness h;
    h.publish();
    auto null = h.request(Request::WRITE, 0, 4096, 16);
    null.source = nullptr;
    auto overflow = h.request(Request::WRITE, 0, 4096, 16);
    overflow.target_offset = std::numeric_limits<uint64_t>::max();
    auto bad_op = h.request(Request::WRITE, 0, 4096, 16);
    bad_op.opcode = static_cast<Request::OpCode>(99);
    auto zero = h.request(Request::READ, 0, 0, 0);
    zero.source = nullptr;
    auto id = h.submit({null, overflow, bad_op, zero,
                        h.request(Request::READ, 16380, 4096, 16)});
    assert(h.status(id).s == Transport::FAILED);
    assert(mock::state.opens == 0 && mock::state.copies == 0 &&
           mock::state.events.empty());
    h.free(id);
}
void failedUnregisterRetainsRegistration() {
    Harness h;
    h.publish();
    assert(TransferEngine::registerBuffer(*h.luca, h.source.data, 64) == 0);
    auto id = h.submit({h.request(Request::WRITE, 0, 4096, 32)});
    mock::state.fail_pending_drain = 1;
    assert(TransferEngine::unregisterBuffer(*h.luca, h.source.data) != 0);
    assert(h.metadata->segments[0]->buffers.size() == 2);
    assert(mock::state.closes == 0);
    assert(h.status(id).s == Transport::FAILED);
    assert(TransferEngine::unregisterBuffer(*h.luca, h.source.data) == 0);
    h.free(id);
}
void eventAllocationFailure() {
    Harness h;
    h.publish();
    mock::state.fail_event_create = 1;
    auto id = h.submit({h.request(Request::WRITE, 0, 4096, 32)});
    assert(h.status(id).s == Transport::FAILED);
    assert(mock::state.copies == 0 && mock::state.closes == 1);
    h.free(id);
}
void invalidOptions() {
    std::vector<std::pair<const char*, const char*>> options{
        {"unknown", nullptr},
        {"auto", "-1"},
        {"auto", " 1"},
        {"auto", "184467440737095516160"}};
    for (const auto& option : options) {
        env("MC_LUCA_COPY_API", option.first);
        env("MC_LUCA_BATCHFLAG_MIN_BYTES", option.second);
        std::string name = "invalid";
        auto metadata = std::make_shared<TransferMetadata>();
        MultiTransport engine(metadata, name);
        assert(!engine.installTransport("luca", std::make_shared<Topology>()));
    }
    assert(mock::state.streams.empty() && mock::state.opens == 0);
}
int main() {
    std::vector<std::pair<std::string, std::function<void()>>> cases{
        {"async READ/WRITE, guards, pending and repeat poll",
         [] { correctCopies("default"); }},
        {"batchflag descriptors live through completion",
         [] { correctCopies("batchflag"); }},
        {"failed task does not end an in-flight batch", partialFailureWaits},
        {"per-batch events and in-flight mapping reuse", overlappingBatches},
        {"host guard", [] { badEnvelope("host_id"); }},
        {"ABI guard", [] { badEnvelope("abi_size"); }},
        {"allocation range guard", [] { badEnvelope("allocation_size"); }},
        {"handle decoding guard", [] { badEnvelope("handle_hex"); }},
        {"format guard", [] { badEnvelope("format"); }},
        {"partial memcpy enqueue drains", [] { enqueueFailure(false, false); }},
        {"partial batchflag enqueue drains",
         [] { enqueueFailure(true, false); }},
        {"failed drain retains buffers and refuses free",
         [] { enqueueFailure(false, true); }},
        {"event-record failure drains", [] { eventFailure(true); }},
        {"event-query failure drains", [] { eventFailure(false); }},
        {"registration duplicate and publication rollback",
         registrationTransactions},
        {"unregister waits for in-flight copies", unregisterDrains},
        {"mixed auto ordering", mixedAutoOrdering},
        {"timeout drains before failure", timeoutDrains},
        {"route rejection leaves batch unchanged", rejectedRouteRollsBack},
        {"counter-driven backend unchanged", counterBackendUnchanged},
        {"zero-byte and local requests", zeroAndLocal},
        {"failed mapping close retained for teardown", closeFailureRetained},
        {"direct submit, capacity and task-index checks", directSubmission},
        {"null, invalid opcode, overflow and local bounds", invalidRequests},
        {"failed unregister retains registration",
         failedUnregisterRetainsRegistration},
        {"event allocation failure before enqueue", eventAllocationFailure},
        {"invalid copy options fail installation", invalidOptions},
    };
    for (auto& entry : cases) {
        mock::reset();
        entry.second();
        assert(mock::state.unsafe_closes == 0);
        assert(mock::state.streams.empty() && mock::state.events.empty());
        std::cout << "PASS " << entry.first << '\n';
    }
    std::cout << "LUCA_MOCK_CASES_PASS=" << cases.size() << '\n';
}

#include "common/runner.h"
#include <future>

namespace dsnet {

Runner::Runner(int nb_worker_thread) 
    : worker_pool(nb_worker_thread), solo_thread(1)
{
}

Runner::~Runner() {
    // TODO dump latency
}

using Solo = std::function<void ()>;
using std::promise;
using std::move;
using std::future;

void Runner::RunPrologue(Prologue prologue) {
    promise<Solo> solo_promise;
    promise<void> release_worker;
    future<void> release = release_worker.get_future();
    solo_thread.push([
        solo_future = solo_promise.get_future(),
        release_worker = move(release_worker)
    ](int id) mutable {
        release_worker.set_value();
        Solo solo = solo_future.get();
        if (solo) {
            solo();
        }
    });
    worker_pool.push([
        prologue, 
        solo_promise = move(solo_promise),
        release = move(release)
    ](int id) mutable {
        Solo solo = prologue();
        solo_promise.set_value(solo);
        release.get();
    });
}

void Runner::RunEpilogue(Epilogue epilogue) {
    worker_pool.push([epilogue](int id) {
        epilogue();
    });
}

}
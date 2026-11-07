#include <iostream>
#include <cassert>
#include <chrono>
#include <thread>
#include "SimulationThread.h"
#include "InputQueue.h"
#include "SnapshotQueue.h"

int main() {
    InputQueue inQ;
    SnapshotQueue snapQ;
    SimulationThread sim(inQ, snapQ);
    sim.start();

    // Concurrently push events, inputs, and pop snapshots
    std::thread producer([&]() {
        for (uint32_t i = 1; i <= 10; i++) {
            PlayerEvent ev{PlayerEvent::Type::JOIN, i, (int)i, (uint64_t)i};
            inQ.pushEvent(ev);
            for (uint64_t seq = 1; seq <= 50; seq++) {
                PlayerInput in{i, seq, {true, false, false, false}};
                inQ.pushInput(in);
                std::this_thread::sleep_for(std::chrono::microseconds(500));
            }
            PlayerEvent leaveEv{PlayerEvent::Type::LEAVE, i, (int)i, (uint64_t)i};
            inQ.pushEvent(leaveEv);
        }
    });

    std::thread consumer([&]() {
        for (int i = 0; i < 50; i++) {
            WorldSnapshot snap;
            snapQ.popLatest(snap);
            (void)sim.getTick();
            (void)sim.getTickRate();
            (void)sim.getSnapshotRate();
            (void)sim.getPlayerCount();
            (void)inQ.getHighWaterEvents();
            (void)inQ.getHighWaterInputs();
            (void)snapQ.getHighWaterMark();
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
    });

    producer.join();
    consumer.join();
    sim.stop();
    std::cout << "TSAN CONCURRENCY AUDIT COMPLETED CLEANLY!" << std::endl;
    return 0;
}

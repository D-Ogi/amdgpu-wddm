// SPDX-License-Identifier: MIT
// flip-state-property-test.cpp - the flip state machine of driver/kmd/flip_state.h as a command sequence.
//
// driver/kmd/test/flip_state_test.c drives every edge of that header one call at a time, and
// driver/kmd/test/run_flip_state.ps1 breaks each rule in a copy of the header and requires that test to fail. Both
// are per-decision: they answer "is this edge right for these facts". Neither answers "is the machine right over a
// sequence", and the faults that reached the lab were sequences: a writer that ended while a deposit was open, a
// fault that arrived between two flips, an owed flip that no report carried.
//
// This test generates sequences. The state is the facts wddm.c keeps under the interrupt lock L
// (docs/design/vidpn-flip.md section 7.2); each command is one thing that happens to the pipe, and it calls the real
// decision function of the header and then moves the state. One transition function serves both the model and the
// system under test, so there is no second copy of the machine here to disagree with the first.
//
// What the commands assert, over any sequence RapidCheck builds:
//
//   I1 the generation is odd exactly while a writer holds the primary, so edge F1 (OVERLAP) is unreachable. The
//      header calls F1 the state that facts M904 says cannot happen; this states it over sequences instead of
//      trusting it.
//   I2 a fault absorbs: after E5 every flip answers F0 and every writer's Begin answers FAULTING.
//   I3 an active writer protects the primary: an admitted flip during a writer is one of the three deposits and
//      never E (PROGRAM), whatever the flip asked for.
//   I4 a writer's End that owes work answers FAULT when the OTG is not running, and only then.
//   I5 after an End that programmed, nothing is owed and no deposit is open.
//   I6 the E5 drain is bounded: the step never says "read again" at or past BC250_FLIP_DRAIN_READS.
//
// The seed is fixed by run_flip_state_property.ps1 (RC_PARAMS), so the quick gate runs the same sequences on every
// machine and a failure in the log is reproducible.
#include "driver/kmd/flip_state.h"
#include <rapidcheck.h>
#include <rapidcheck/state.h>
#include <cstdio>
#include <ostream>

namespace {

// The facts of the pipe. PrimarySequence is a counter in the driver; only its parity decides an edge, so the parity
// is what this state keeps.
struct FlipFacts {
    bool faulting = false;      // E5 was entered on this start
    bool writerActive = false;  // a writer is between Begin and End
    bool generationOdd = false; // the primary generation (PrimarySequence) is odd
    bool deposit = false;       // a request was deposited during the current writer, and no unblank programmed it
    bool owed = false;          // a next-VSync request answered success that no CRTC_VSYNC report carried yet
    int otgRunning = 1;         // 1 running, 0 stopped, anything else: the read failed
};

// What one command does to the facts. Written once and called by both apply (the model) and run (the system), so
// the state machine of this test cannot drift from itself.
enum class Step { BeginWriter, EndWriter, Flip, VSyncReport, EnterFault, SetOtg };

struct Event {
    Step step = Step::Flip;
    // A flip request, as the DDI delivers it.
    bool immediate = false;
    bool admitted = true;
    bool drained = false;
    bool resumed = false;
    // The outcome of the write sequence that edge E runs.
    bool wrote = true;
    bool writeOk = true;
    bool proof = true;
    int otg = 1;
};

void advance(FlipFacts &s, const Event &e) {
    switch (e.step) {
        case Step::BeginWriter:
            // A writer never starts in FAULTING, and the caller retries another writer's odd generation. Both
            // answers leave the facts alone; only TAKE changes them.
            if (Bc250WriterBegin(s.faulting, s.generationOdd) != BC250_WRITER_BEGIN_TAKE) return;
            s.writerActive = true;
            s.generationOdd = true;
            s.deposit = false;
            return;
        case Step::EndWriter: {
            const unsigned answer = Bc250WriterEnd(s.deposit, s.owed, s.otgRunning);
            if (answer == BC250_WRITER_END_FAULT) {
                s.faulting = true;
            } else if (answer == BC250_WRITER_END_PROGRAM) {
                // End programmed Desired with the next-VSync type: the deposit is placed and nothing is owed.
                s.deposit = false;
                s.owed = false;
            }
            s.writerActive = false;
            s.generationOdd = false;
            return;
        }
        case Step::Flip: {
            BC250_FLIP_FACTS facts;
            facts.Faulting = s.faulting;
            facts.GenerationOdd = s.generationOdd;
            facts.WriterActive = s.writerActive;
            facts.Admitted = e.admitted;
            facts.Immediate = e.immediate;
            facts.Drained = e.drained;
            facts.Resumed = e.resumed;
            const unsigned edge = Bc250FlipEdge(&facts);
            if (edge == BC250_FLIP_EDGE_FAULTING || edge == BC250_FLIP_EDGE_OVERLAP) {
                s.faulting = true;
                return;
            }
            if (edge == BC250_FLIP_EDGE_REFUSED) return;  // A: nothing published, not a pipe state
            if (edge == BC250_FLIP_EDGE_DEPOSIT_VSYNC || edge == BC250_FLIP_EDGE_DEPOSIT_DRAINED ||
                edge == BC250_FLIP_EDGE_DEPOSIT_UNPROVEN) {
                s.deposit = true;
                return;
            }
            // Edge E ran the write sequence.
            const unsigned result = Bc250FlipResult(e.wrote, e.writeOk, e.immediate, e.proof);
            if (result == BC250_FLIP_RESULT_FAULT_PROGRAM || result == BC250_FLIP_RESULT_UNPROVEN) {
                s.faulting = true;
            } else if (result == BC250_FLIP_RESULT_SUCCESS && !e.immediate) {
                s.owed = true;  // a next-VSync flip: a report must still carry it
            }
            return;
        }
        case Step::VSyncReport:
            // A real CRTC_VSYNC report carries the owed flip. Only a running OTG makes one.
            if (s.otgRunning == 1) s.owed = false;
            return;
        case Step::EnterFault:
            s.faulting = true;
            return;
        case Step::SetOtg:
            s.otgRunning = e.otg;
            return;
    }
}

using FlipCommand = rc::state::Command<FlipFacts, FlipFacts>;

// One command per thing that happens to the pipe. Each one asserts the invariants that its own answer must satisfy,
// then moves the system with the shared transition.
struct FlipRequest : FlipCommand {
    Event event;

    FlipRequest() {
        event.step = Step::Flip;
        event.immediate = *rc::gen::arbitrary<bool>();
        event.admitted = *rc::gen::weightedElement<bool>({{4, true}, {1, false}});
        event.drained = *rc::gen::arbitrary<bool>();
        event.resumed = *rc::gen::arbitrary<bool>();
        event.wrote = *rc::gen::weightedElement<bool>({{4, true}, {1, false}});
        event.writeOk = *rc::gen::weightedElement<bool>({{4, true}, {1, false}});
        event.proof = *rc::gen::weightedElement<bool>({{4, true}, {1, false}});
    }

    void apply(FlipFacts &state) const override { advance(state, event); }

    void run(const FlipFacts &state, FlipFacts &sut) const override {
        BC250_FLIP_FACTS facts;
        facts.Faulting = state.faulting;
        facts.GenerationOdd = state.generationOdd;
        facts.WriterActive = state.writerActive;
        facts.Admitted = event.admitted;
        facts.Immediate = event.immediate;
        facts.Drained = event.drained;
        facts.Resumed = event.resumed;
        const unsigned edge = Bc250FlipEdge(&facts);
        RC_ASSERT(edge < BC250_FLIP_EDGES);
        // I1. The lock keeps the generation parity and the writer flag together, so F1 is unreachable.
        RC_ASSERT(state.generationOdd == state.writerActive);
        RC_ASSERT(edge != BC250_FLIP_EDGE_OVERLAP);
        // I2. A fault absorbs every later request.
        if (state.faulting) {
            RC_ASSERT(edge == BC250_FLIP_EDGE_FAULTING);
        } else if (!event.admitted) {
            RC_ASSERT(edge == BC250_FLIP_EDGE_REFUSED);
        } else if (state.writerActive) {
            // I3. An admitted flip during a writer never programs the plane.
            RC_ASSERT(edge == BC250_FLIP_EDGE_DEPOSIT_VSYNC || edge == BC250_FLIP_EDGE_DEPOSIT_DRAINED ||
                      edge == BC250_FLIP_EDGE_DEPOSIT_UNPROVEN);
        } else {
            RC_ASSERT(edge == BC250_FLIP_EDGE_PROGRAM);
        }
        advance(sut, event);
    }

    void show(std::ostream &os) const override {
        os << "Flip(immediate=" << event.immediate << " admitted=" << event.admitted << " drained=" << event.drained
           << " resumed=" << event.resumed << " wrote=" << event.wrote << " writeOk=" << event.writeOk
           << " proof=" << event.proof << ")";
    }
};

struct BeginWriter : FlipCommand {
    void checkPreconditions(const FlipFacts &state) const override { RC_PRE(!state.writerActive); }

    void apply(FlipFacts &state) const override {
        Event e;
        e.step = Step::BeginWriter;
        advance(state, e);
    }

    void run(const FlipFacts &state, FlipFacts &sut) const override {
        const unsigned answer = Bc250WriterBegin(state.faulting, state.generationOdd);
        RC_ASSERT(answer < BC250_WRITER_BEGINS);
        // I2. After E5 no writer takes the primary.
        if (state.faulting) {
            RC_ASSERT(answer == BC250_WRITER_BEGIN_FAULTING);
        } else {
            // I1 again: with no writer active the generation is even, so Begin takes it rather than retrying.
            RC_ASSERT(!state.generationOdd);
            RC_ASSERT(answer == BC250_WRITER_BEGIN_TAKE);
        }
        Event e;
        e.step = Step::BeginWriter;
        advance(sut, e);
    }

    void show(std::ostream &os) const override { os << "BeginWriter"; }
};

struct EndWriter : FlipCommand {
    void checkPreconditions(const FlipFacts &state) const override { RC_PRE(state.writerActive); }

    void apply(FlipFacts &state) const override {
        Event e;
        e.step = Step::EndWriter;
        advance(state, e);
    }

    void run(const FlipFacts &state, FlipFacts &sut) const override {
        const unsigned answer = Bc250WriterEnd(state.deposit, state.owed, state.otgRunning);
        RC_ASSERT(answer < BC250_WRITER_ENDS);
        // I4. Work to place and a pipe that cannot report it is the one fault of End.
        const bool work = state.deposit || state.owed;
        if (!work) {
            RC_ASSERT(answer == BC250_WRITER_END_NOTHING);
        } else if (state.otgRunning != 1) {
            RC_ASSERT(answer == BC250_WRITER_END_FAULT);
        } else {
            RC_ASSERT(answer == BC250_WRITER_END_PROGRAM);
        }
        Event e;
        e.step = Step::EndWriter;
        advance(sut, e);
        // I5. An End that programmed leaves nothing owed and no deposit open.
        if (answer == BC250_WRITER_END_PROGRAM) {
            RC_ASSERT(!sut.owed);
            RC_ASSERT(!sut.deposit);
        }
        // The writer let the primary go, whatever the answer was.
        RC_ASSERT(!sut.writerActive);
        RC_ASSERT(!sut.generationOdd);
    }

    void show(std::ostream &os) const override { os << "EndWriter"; }
};

struct VSyncReport : FlipCommand {
    void apply(FlipFacts &state) const override {
        Event e;
        e.step = Step::VSyncReport;
        advance(state, e);
    }

    void run(const FlipFacts &, FlipFacts &sut) const override {
        Event e;
        e.step = Step::VSyncReport;
        advance(sut, e);
    }

    void show(std::ostream &os) const override { os << "VSyncReport"; }
};

struct EnterFault : FlipCommand {
    void apply(FlipFacts &state) const override {
        Event e;
        e.step = Step::EnterFault;
        advance(state, e);
    }

    void run(const FlipFacts &, FlipFacts &sut) const override {
        Event e;
        e.step = Step::EnterFault;
        advance(sut, e);
    }

    void show(std::ostream &os) const override { os << "EnterFault"; }
};

struct SetOtg : FlipCommand {
    Event event;

    // 1 running, 0 stopped, 2 stands for a read that failed: the header treats anything but 1 as "cannot report".
    SetOtg() {
        event.step = Step::SetOtg;
        event.otg = *rc::gen::element(0, 1, 2);
    }

    void apply(FlipFacts &state) const override { advance(state, event); }

    void run(const FlipFacts &, FlipFacts &sut) const override { advance(sut, event); }

    void show(std::ostream &os) const override { os << "SetOtg(" << event.otg << ")"; }
};

}  // namespace

int main() {
    bool ok = true;

    ok = rc::check("the flip state machine holds its invariants over any sequence", [] {
             FlipFacts initial;
             FlipFacts sut;
             rc::state::check(initial, sut,
                              rc::state::gen::execOneOfWithArgs<FlipRequest, BeginWriter, EndWriter, VSyncReport,
                                                                EnterFault, SetOtg>());
         }) && ok;

    // I6. The E5 drain of the DPC is bounded. A poll at DIRQL that never ends is the fault this rule prevents, and
    // a sequence test cannot reach it: the bound is a property of the one step function.
    ok = rc::check("the E5 drain step is bounded", [] {
             const unsigned reads = *rc::gen::inRange<unsigned>(0, BC250_FLIP_DRAIN_READS * 2);
             const bool read = *rc::gen::arbitrary<bool>();
             const bool drained = *rc::gen::arbitrary<bool>();
             const unsigned step = Bc250FlipDrainStep(read, drained, reads);
             RC_ASSERT(step < BC250_FLIP_DRAINS);
             if (!read) {
                 RC_ASSERT(step == BC250_FLIP_DRAIN_UNKNOWN);
             } else if (drained) {
                 RC_ASSERT(step == BC250_FLIP_DRAIN_DRAINED);
             } else if (reads >= BC250_FLIP_DRAIN_READS) {
                 RC_ASSERT(step == BC250_FLIP_DRAIN_UNKNOWN);
             } else {
                 RC_ASSERT(step == BC250_FLIP_DRAIN_AGAIN);
             }
             // The loop ends: no answer past the bound says "read again".
             RC_ASSERT(!(reads >= BC250_FLIP_DRAIN_READS && step == BC250_FLIP_DRAIN_AGAIN));
         }) && ok;

    // The W-u classification is total, and a fault never reads as a programmed deposit.
    ok = rc::check("the W-u record classifies every End", [] {
             const bool unblank = *rc::gen::arbitrary<bool>();
             const bool current = *rc::gen::arbitrary<bool>();
             const unsigned answer = *rc::gen::inRange<unsigned>(0, BC250_WRITER_ENDS);
             const unsigned result = *rc::gen::inRange<unsigned>(0, BC250_FLIP_RESULTS);
             const unsigned record = Bc250WuEnd(unblank, current, answer, result);
             RC_ASSERT(record < BC250_WU_ENDS);
             if (!unblank && current && answer == BC250_WRITER_END_FAULT) RC_ASSERT(record == BC250_WU_END_FAULT);
             if (record == BC250_WU_END_PROGRAM) {
                 RC_ASSERT(answer == BC250_WRITER_END_PROGRAM);
                 RC_ASSERT(result == BC250_FLIP_RESULT_SUCCESS);
             }
         }) && ok;

    std::printf("flip-state-property: %s\n", ok ? "PASS" : "FAIL");
    return ok ? 0 : 1;
}

"""Host checks for request-correlated presentation diagnostics.

This model does not drive presentation or execute EE/GS code. Marker times are
IRQ observations, not hardware timestamps; queue time precedes DMA Send/cache
writeback. Unsigned elapsed arithmetic handles a crossed CP0 wrap only when
less than one full Count period elapsed. An unobserved full wrap is unknowable.
"""

from copy import deepcopy
from pathlib import Path
import re
import unittest

from presentation_sync_model import source_function, source_with_metric_gates


ROOT = Path(__file__).resolve().parents[1]
U32 = 0xffffffff
CYCLES_PER_MS = 294_912
MAX_SAMPLE_CYCLES = 1000 * CYCLES_PER_MS
REJECT_MODE, REJECT_REWIND, REJECT_PHASE, REJECT_CLOCK, REJECT_FIELD = (1, 2, 4, 8, 16)


def elapsed(now, before):
    return (now - before) & U32


def bucket(cycles):
    return 0 if cycles < CYCLES_PER_MS else 1 if cycles < 3 * CYCLES_PER_MS else 2


def empty_group():
    return dict.fromkeys(("samples", "signal", "finish", "ready", "wait", "post",
                          "budget", "post_samples", "budget_samples",
                          "max_ready", "max_wait"), 0)


def empty_report():
    return dict(groups=[empty_group(), empty_group()], reject=[0] * 5,
                joint=[0] * 9, unknown=0, discarded=0, worst=None)


class Timeline:
    """One diagnostic owner; independent of the renderer's permission state."""

    def __init__(self, cutoff=576):
        self.cutoff = cutoff
        self.enabled = False
        self.active = None
        self.previous_publication = None
        self.report = empty_report()

    def enable(self, enabled):
        self.enabled = enabled
        self.active = None
        self.previous_publication = None
        self.report = empty_report()

    def layout_reset(self):
        if self.enabled and self.active is not None:
            self.report["discarded"] += 1
        self.active = None
        self.previous_publication = None

    def snapshot(self):
        result = deepcopy(self.report)
        self.report = empty_report()
        return result

    def queue(self, seq, now, phase, normal_pending=True, cadence=1):
        if not self.enabled or not normal_pending or cadence != 1:
            return
        assert self.active is None
        post = None
        if self.previous_publication is not None:
            duration = elapsed(now, self.previous_publication)
            if duration < MAX_SAMPLE_CYCLES:
                post = duration
        self.active = dict(seq=seq, queue=now, queue_phase=phase, signal=None,
                           finish=None, ready=None, ready_phase=None,
                           edge_phase=None, budget=None, edges=0, missed=0,
                           reject=0, post=post)

    def events(self, seq, now, phase, signal=False, finish=False, normal=True):
        sample = self.active
        if not normal or sample is None or seq != sample["seq"]:
            return
        # Co-observed markers receive exactly the same entry time. No sample
        # can invent an ordering from the handler's FINISH/SIGNAL branch order.
        if signal and sample["signal"] is None:
            sample["signal"] = now
        if finish and sample["finish"] is None:
            sample["finish"] = now
        if (sample["ready"] is None and sample["signal"] is not None
                and sample["finish"] is not None):
            sample["ready"] = now
            sample["ready_phase"] = phase

    def edge(self, now, phase, admitted=True):
        sample = self.active
        if sample is None:
            return
        if sample["edges"] == 0:
            sample["edge_phase"] = phase
            if admitted:
                sample["budget"] = elapsed(now, sample["queue"]) + (self.cutoff - phase) * 512
        else:
            sample["missed"] += 1
        sample["edges"] += 1

    def reject(self, mask):
        if not self.enabled:
            return
        for index in range(5):
            self.report["reject"][index] += bool(mask & (1 << index))
        if self.active is not None:
            self.active["reject"] |= mask

    def present(self, seq, now):
        if not self.enabled:
            return
        sample = self.active
        self.previous_publication = now
        if sample is None or sample["seq"] != seq:
            return
        self.active = None
        total = elapsed(now, sample["queue"])
        if (sample["ready"] is None or sample["edges"] == 0 or sample["edges"] >= 64
                or total >= MAX_SAMPLE_CYCLES):
            self.report["discarded"] += 1
            return
        for name in ("signal", "finish", "ready"):
            sample[name] = elapsed(sample[name], sample["queue"])
        sample["wait"] = total - sample["ready"]
        sample["total"] = total
        group = self.report["groups"][int(sample["missed"] > 0)]
        group["samples"] += 1
        for name in ("signal", "finish", "ready", "wait"):
            group[name] += sample[name]
        for name in ("post", "budget"):
            if sample[name] is not None:
                group[name] += sample[name]
                group[name + "_samples"] += 1
        group["max_ready"] = max(group["max_ready"], sample["ready"])
        group["max_wait"] = max(group["max_wait"], sample["wait"])
        if sample["missed"]:
            if sample["budget"] is None:
                self.report["unknown"] += 1
            else:
                self.report["joint"][3 * bucket(sample["budget"]) + bucket(sample["ready"])] += 1
        worst = self.report["worst"]
        if sample["missed"] and (worst is None or sample["total"] > worst["total"]):
            self.report["worst"] = deepcopy(sample)


class TimelineContractTests(unittest.TestCase):
    def timeline(self):
        model = Timeline()
        model.enable(True)
        return model

    def test_bootstrap_and_immediate_uploads_are_not_samples(self):
        model = self.timeline()
        model.queue(0, 100, 0, normal_pending=False)
        model.events(0, 200, 0, signal=True, normal=False)
        model.present(0, 300)
        self.assertIsNone(model.active)
        self.assertEqual(model.report["groups"][0]["samples"], 0)
        model.queue(1, 400, 0)
        model.events(1, 500, 0, signal=True, normal=False)
        self.assertIsNone(model.active["signal"])

    def test_wrong_owner_and_duplicate_events_do_not_change_original_times(self):
        model = self.timeline()
        model.queue(9, 100, 0)
        model.events(8, 200, 0, signal=True, finish=True)
        self.assertIsNone(model.active["ready"])
        model.events(9, 300, 20, signal=True)
        model.events(9, 900, 21, signal=True)
        self.assertEqual(model.active["signal"], 300)
        self.assertIsNone(model.active["ready"])
        model.events(9, 1000, 22, finish=True)
        self.assertEqual(model.active["ready"], 1000)
        self.assertEqual(model.active["ready_phase"], 22)

    def test_marker_order_and_coobserved_markers_preserve_latency(self):
        for first in ("signal", "finish", "both"):
            with self.subTest(first=first):
                model = self.timeline()
                model.queue(1, 1000, 500)
                if first == "both":
                    model.events(1, 4000, 100, signal=True, finish=True)
                else:
                    model.events(1, 3000, 90, **{first: True})
                    model.events(1, 4000, 100, **{"finish" if first == "signal" else "signal": True})
                model.edge(4500, 110)
                model.present(1, 5000)
                group = model.report["groups"][0]
                self.assertEqual(group["ready"], 3000)
                self.assertEqual(group["wait"], 1000)
                self.assertEqual(group["samples"], 1)
                if first == "both":
                    self.assertEqual(group["signal"], group["finish"])

    def test_initially_unready_rescue_is_not_a_missed_request(self):
        model = self.timeline()
        model.queue(1, 1000, 500)
        model.edge(2000, 130)
        model.events(1, 3000, 132, signal=True, finish=True)
        model.present(1, 3100)
        self.assertEqual(model.report["groups"][0]["samples"], 1)
        self.assertEqual(model.report["groups"][1]["samples"], 0)

    def test_second_pending_edge_marks_miss_without_replacing_first_budget(self):
        model = self.timeline()
        model.queue(1, 1000, 500)
        model.edge(2000, 130)
        first_budget = 1000 + (576 - 130) * 512
        model.edge(5_000_000, 140)
        model.events(1, 5_001_000, 142, signal=True, finish=True)
        model.present(1, 5_002_000)
        sample = model.report["worst"]
        self.assertEqual(sample["missed"], 1)
        self.assertEqual(sample["budget"], first_budget)
        self.assertEqual(model.report["joint"], [0, 0, 1, 0, 0, 0, 0, 0, 0])

    def test_rejected_first_edge_keeps_budget_unknown(self):
        model = self.timeline()
        model.queue(1, 1000, 500)
        model.edge(2000, 700, admitted=False)
        model.edge(5_000_000, 130)
        model.events(1, 5_001_000, 131, signal=True, finish=True)
        model.present(1, 5_002_000)
        self.assertEqual(model.report["unknown"], 1)
        self.assertEqual(model.report["joint"], [0] * 9)
        self.assertEqual(model.report["groups"][1]["budget_samples"], 0)

    def test_snapshot_keeps_one_whole_correlated_record(self):
        model = self.timeline()
        model.queue(1, 1000, 0)
        model.events(1, 2000, 0, signal=True)
        first = model.snapshot()
        self.assertEqual(first["groups"][0]["samples"], 0)
        model.edge(2500, 130)
        model.events(1, 3000, 131, finish=True)
        second = model.snapshot()
        self.assertEqual(second["groups"][0]["samples"], 0)
        model.present(1, 4000)
        third = model.snapshot()
        self.assertEqual(third["groups"][0]["samples"], 1)
        self.assertEqual(third["groups"][0]["signal"], 1000)
        self.assertEqual(third["groups"][0]["finish"], 2000)
        self.assertEqual(third["groups"][0]["wait"], 1000)

    def test_enable_and_layout_reset_invalidate_active_and_previous_clock(self):
        for operation in ("enable", "layout"):
            with self.subTest(operation=operation):
                model = self.timeline()
                model.present(0, 100)
                model.queue(1, 200, 0)
                model.events(1, 300, 0, signal=True)
                model.enable(True) if operation == "enable" else model.layout_reset()
                self.assertIsNone(model.active)
                self.assertIsNone(model.previous_publication)
                model.queue(2, 400, 0)
                self.assertIsNone(model.active["post"])

    def test_crossed_count_wrap_preserves_small_owned_intervals(self):
        model = self.timeline()
        model.present(0, U32 - 3000)
        model.queue(0, U32 - 1000, 500)
        model.events(0, 1999, 100, signal=True, finish=True)
        model.edge(2499, 101)
        model.present(0, 3999)
        group = model.report["groups"][0]
        self.assertEqual((group["ready"], group["wait"], group["post"]), (3000, 2000, 2000))

    def test_reject_events_are_independent_of_per_request_accumulated_mask(self):
        model = self.timeline()
        model.queue(1, 1000, 0)
        model.events(1, 2000, 100, signal=True, finish=True)
        model.reject(REJECT_PHASE | REJECT_CLOCK)
        model.reject(REJECT_PHASE | REJECT_FIELD)
        model.edge(5_000_000, 130)
        model.present(1, 5_000_100)
        self.assertEqual(model.report["reject"], [0, 0, 2, 1, 1])

    def test_large_or_many_edge_samples_are_discarded(self):
        for duration, edges, discarded in ((MAX_SAMPLE_CYCLES - 1, 1, 0),
                                           (MAX_SAMPLE_CYCLES, 1, 1),
                                           (MAX_SAMPLE_CYCLES + 1, 1, 1),
                                           (5000, 63, 0), (5000, 64, 1)):
            with self.subTest(duration=duration, edges=edges):
                model = self.timeline()
                model.queue(1, 1000, 0)
                model.events(1, 2000, 0, signal=True, finish=True)
                for _ in range(edges):
                    model.edge(3000, 130)
                model.present(1, (1000 + duration) & U32)
                self.assertEqual(model.report["discarded"], discarded)
                self.assertEqual(sum(g["samples"] for g in model.report["groups"]), 1 - discarded)

    def test_joint_histogram_boundaries_are_exact(self):
        values = (0, CYCLES_PER_MS - 1, CYCLES_PER_MS,
                  3 * CYCLES_PER_MS - 1, 3 * CYCLES_PER_MS)
        self.assertEqual([bucket(value) for value in values], [0, 0, 1, 1, 2])
        self.assertEqual([3 * row + column for row in range(3) for column in range(3)],
                         list(range(9)))

    def test_disabled_collection_does_not_create_samples(self):
        model = Timeline()
        model.queue(1, 1000, 0)
        model.events(1, 2000, 100, signal=True, finish=True)
        model.edge(3000, 130)
        model.present(1, 4000)
        self.assertEqual(model.report, empty_report())
        self.assertIsNone(model.active)
        self.assertIsNone(model.previous_publication)

    def test_intentional_multi_refresh_cadence_has_no_normal_sample(self):
        for cadence in (2, 4):
            with self.subTest(cadence=cadence):
                model = self.timeline()
                model.queue(1, 1000, 0, cadence=cadence)
                model.edge(2000, 130)
                model.edge(5_000_000, 130)
                model.events(1, 5_000_100, 130, signal=True, finish=True)
                model.present(1, 5_000_200)
                self.assertEqual(sum(g["samples"] for g in model.report["groups"]), 0)
                self.assertIsNone(model.report["worst"])

    def test_stale_previous_publication_omits_post_without_losing_new_sample(self):
        model = self.timeline()
        model.present(0, 1000)
        model.queue(1, 1000 + MAX_SAMPLE_CYCLES, 0)
        model.events(1, 2000 + MAX_SAMPLE_CYCLES, 100, signal=True, finish=True)
        model.edge(2500 + MAX_SAMPLE_CYCLES, 130)
        model.present(1, 3000 + MAX_SAMPLE_CYCLES)
        group = model.report["groups"][0]
        self.assertEqual(group["samples"], 1)
        self.assertEqual(group["post_samples"], 0)
        self.assertEqual(model.report["discarded"], 0)

    def test_worst_is_a_whole_missed_record_not_independent_component_maxima(self):
        model = self.timeline()
        for seq, ready, total, miss in ((1, 1000, 9000, True),
                                        (2, 5000, 8000, True),
                                        (3, 8000, 15000, False)):
            start = seq * 100_000
            model.queue(seq, start, 0)
            model.edge(start + 100, 130)
            if miss:
                model.edge(start + 500, 130)
            model.events(seq, start + ready, 140, signal=True, finish=True)
            model.present(seq, start + total)
        worst = model.report["worst"]
        self.assertEqual((worst["seq"], worst["ready"], worst["wait"]), (1, 1000, 8000))
        self.assertEqual(model.report["groups"][1]["max_ready"], 5000)
        self.assertEqual(model.report["groups"][0]["samples"], 1)


class TimelineSourceContractTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.gl = (ROOT / "src/glcontext.cpp").read_text(encoding="utf-8")
        cls.header = (ROOT / "include/GL/ps2gl.h").read_text(encoding="utf-8")

    def test_header_preserves_duration_width_and_optional_value_flags(self):
        self.assertIn("#define PGL_PRESENT_TIMELINE_METRICS 1", self.header)
        for name in ("queueToSignalCycles", "queueToFinishCycles", "queueToReadyCycles",
                     "readyToPresentCycles", "postPresentToQueueCycles", "budgetCycles"):
            self.assertRegex(self.header, rf"pglU64_t [^;]*\b{name}\b")
        self.assertIn("PGL_PRESENT_TRACE_HAS_POST = 1", self.header)
        self.assertIn("PGL_PRESENT_TRACE_HAS_BUDGET = 2", self.header)
        self.assertIn("PGLPresentationTimingGroup groups[2];", self.header)
        self.assertIn("unsigned int missedJoint[9]", self.header)
        self.assertIn("pglTakePresentationTimingMetrics(PGLPresentationTimingStats* stats)", self.header)

    def test_record_arming_is_owned_guarded_and_does_not_sample_bootstrap(self):
        arm = source_function(self.gl, "static void ArmPresentationTiming(")
        self.assertIn("!FramePhaseEnabled || !PresentationEnabled || PresentationIntervals != 1", arm)
        self.assertIn("PresentationTiming.normal = NormalChainsSubmitted;", arm)
        self.assertLess(arm.index("PresentationTiming.queueClock = ReadEeCycleCount();"),
                        arm.index("PresentationTiming.active = true;"))
        queue = source_function(self.gl, "void CGLContext::QueuePresentation(")
        self.assertLess(queue.index("DIntr()"), queue.index("ArmPresentationTiming(request);"))
        self.assertLess(queue.index("PresentationRequested = request;"),
                        queue.index("ArmPresentationTiming(request);"))
        self.assertLess(queue.index("ArmPresentationTiming(request);"), queue.index("EIntr();", queue.index("ArmPresentationTiming(request);")))
        send = source_function(self.gl, "void CGLContext::RenderGeometry()")
        self.assertLess(send.index("QueuePresentation("), send.index("LastPacket->Send();"))

    def test_both_irq_marker_observations_share_one_entry_clock(self):
        irq = source_function(self.gl, "int CGLContext::GsIntHandler(")
        self.assertEqual(irq.count("const unsigned int completionClock"), 1)
        self.assertIn("PresentationTiming.active && (csr & 3) ? ReadEeCycleCount() : 0", irq)
        self.assertIn("ObservePresentationCompletion(true, completionClock);", irq)
        self.assertIn("ObservePresentationCompletion(false, completionClock);", irq)
        helper = source_function(self.gl, "static void ObservePresentationCompletion(")
        self.assertIn("PresentationTiming.request != PresentationRequested", helper)
        self.assertIn("PresentationTiming.normal != NormalChainsSubmitted", helper)
        self.assertIn("if (PresentationTiming.finishSeen) return;", helper)
        self.assertIn("if (PresentationTiming.signalSeen) return;", helper)
        self.assertIn("sample.readyPhase = *R_EE_T1_COUNT;", helper)

    def test_snapshot_does_not_split_or_reset_inflight_record(self):
        snapshot = source_function(self.gl, 'extern "C" GLboolean pglTakePresentationTimingMetrics(')
        self.assertLess(snapshot.index("DIntr()"), snapshot.index("memcpy(stats,"))
        self.assertLess(snapshot.index("memset(&PresentationTimingStats"), snapshot.index("EIntr()"))
        self.assertNotIn("ResetPresentationTimingRecord", snapshot)
        self.assertNotIn("PresentationTiming.active", snapshot)
        reset = source_function(self.gl, "static void ResetPresentationTimingRecord()")
        self.assertIn("PresentationTiming.active = false;", reset)
        self.assertIn("PreviousPresentationClockValid = false;", reset)
        for signature in ('extern "C" GLboolean pglSetFramePhaseMetrics(',
                          "void CGLContext::ResetPresentation()"):
            owner = source_function(self.gl, signature)
            self.assertIn("ResetPresentationTimingRecord();", owner)

    def test_publication_aggregates_whole_samples_with_bounded_cohorts(self):
        helper = source_function(self.gl, "static void CompletePresentationTiming(")
        self.assertIn("total < PresentationTraceMaxCycles", helper)
        self.assertIn("PresentationTiming.edgeSeen && !PresentationTiming.badAge", helper)
        self.assertIn("PresentationTiming.signalSeen && PresentationTiming.finishSeen", helper)
        self.assertIn("sample.missedEdges != 0 ? 1u : 0u", helper)
        self.assertIn("PresentationTimingStats.worst = sample;", helper)
        missed = source_function(helper, "if (groupIndex != 0)")
        self.assertIn("PresentationTimingStats.worst = sample;", missed)
        publish = source_function(self.gl, "void CGLContext::TryPresent(")
        self.assertLess(publish.index("dispfb2 = PresentationFB2"),
                        publish.index("CompletePresentationTiming(clock);"))
        self.assertLess(publish.index("PresentationCompleted = PresentationRequested;"),
                        publish.index("CompletePresentationTiming(clock);"))

    def test_both_diagnostic_gates_compile_away_timeline_without_weakening_pacing(self):
        for frame, timeline in ((0, 0), (0, 1), (1, 0)):
            with self.subTest(frame=frame, timeline=timeline):
                disabled = source_with_metric_gates(self.gl, frame, timeline)
                for symbol in ("ArmPresentationTiming(", "ObservePresentationCompletion(",
                               "ObservePresentationEdge(", "CompletePresentationTiming(",
                               "ResetPresentationTimingRecord(", "completionClock"):
                    self.assertNotIn(symbol, disabled)
                getter = source_function(disabled, 'extern "C" GLboolean pglTakePresentationTimingMetrics(')
                self.assertIn("memset(stats, 0, sizeof(*stats));", getter)
                self.assertIn("return GL_FALSE;", getter)
                publish = source_function(disabled, "void CGLContext::TryPresent(")
                for condition in ("NormalChainsCompleted != PresentationNormalSequence",
                                  "NormalFramesFinished != PresentationNormalSequence",
                                  "phase >= PresentationBlankTicks",
                                  "fieldIsEven != PresentationApertureFieldIsEven"):
                    self.assertIn(condition, publish)
                self.assertIn("dispfb2 = PresentationFB2", publish)


if __name__ == "__main__":
    print("Host diagnostics model only: not EE/GS timing or IRQ-latency validation.")
    unittest.main(verbosity=2)

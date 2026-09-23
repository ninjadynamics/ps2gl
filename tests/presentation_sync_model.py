"""Host event-order model for the PS2 frame-completion/publication contract.

This does not execute EE code, the kernel interrupt dispatcher, or the GS.
The phase model assumes the audited Timer1 BUSCLK/256 reset/count semantics
and a minimum 823-tick blank for supported modes. It does not prove those
hardware facts or the real two-register publication's execution bound. Those
require separate manual review and hardware validation. Semaphore counts are
deliberately unreliable as evidence: only owned sequences authorize progress.
The interval ledger counts serviced VSINT observations, not physical refreshes
when interrupts coalesce. Its open interval closes at the following observation,
so publication and interval counts may straddle a reporting snapshot.
"""

from pathlib import Path
import re
import unittest


ROOT = Path(__file__).resolve().parents[1]
U32 = 0xffffffff
SIGNAL, FINISH, VSYNC = 1, 2, 8
LEGACY_BLANK_TICKS = 288
EARLY_BLANK_TICKS = 576
BLANK_TICKS = 823
FRAME_TICKS = 9600
EE_CYCLES_PER_TICK = 512


def increment(value):
    return (value + 1) & U32


class Presentation:
    """Single normal producer, one pending immutable presentation descriptor."""

    def __init__(self, blank_ticks=EARLY_BLANK_TICKS):
        self.blank_ticks = blank_ticks
        self.submitted = self.command_done = self.raster_done = 0
        self.immediate_submitted = self.immediate_done = 0
        self.requested = self.presented = 0
        self.intervals = 0
        self.csr = 0
        self.signal_kind = None
        self.end_pending = False
        self.blank = False
        self.phase_ticks = BLANK_TICKS
        self.timer_mode = 0x9e
        self.field = self.published_field = 0
        self.tokens = dict(command=0, raster=0, present=0, vsync=0)
        self.front = self.cpu_front = 0
        self.descriptor = None
        self.writes = []
        self.rotations = 0
        self.async_enabled = False
        self.expected_normal = 0
        self.cadence = 1
        self.cp0 = 0
        self.aperture = None
        self.metrics_enabled = False
        self.field_observed = self.field_published = False
        self.repeat_history = self.repeat_count = self.history_fields = 0
        self.metrics = dict.fromkeys((
            "queued", "presented", "atVsync", "afterCompletion",
            "notReadyEdges", "lateEdges", "joinReady", "joinWait",
            "displayFields", "repeatedFields", "emptyQueueEdges",
            "windows30", "worstRepeat30", "extendedBlank", "expiredReady"), 0)

    def set_metrics(self, enabled):
        self.metrics = dict.fromkeys(self.metrics, 0)
        self.field_observed = self.field_published = False
        self.repeat_history = self.repeat_count = self.history_fields = 0
        self.metrics_enabled = enabled

    def take_metrics(self):
        # Snapshot/reset counters under interrupt exclusion, but retain the
        # interval that will only be classified by the next serviced VSINT.
        result = self.metrics.copy()
        self.metrics = dict.fromkeys(self.metrics, 0)
        return result

    def count(self, name):
        if self.metrics_enabled:
            self.metrics[name] = increment(self.metrics[name])

    def finished(self):
        return self.command_done == self.submitted == self.raster_done

    def finish_poll(self):
        # One step of the two predicate-driven waits, including stale wakes.
        for kind, done in (("command", self.command_done),
                           ("raster", self.raster_done)):
            if done != self.submitted:
                if self.tokens[kind]:
                    self.tokens[kind] -= 1
                return False
        return True

    def send_normal(self):
        assert self.finished(), "previous normal frame must retire"
        assert self.requested == self.presented, "frontbuffer still owned by scanout"
        assert self.cpu_front == self.front, "CPU ownership has not rotated"
        self.tokens["command"] = self.tokens["raster"] = 0
        self.csr &= ~FINISH
        self.submitted = increment(self.submitted)
        if self.async_enabled:
            self.request(self.cadence, expected_normal=self.submitted)
        return (0x61, 0x60)  # FINISH followed by SIGNAL, not a blocking barrier.

    def send_immediate(self):
        self.immediate_submitted = increment(self.immediate_submitted)
        return (0x60,)

    def signal(self, kind=1):
        assert not self.csr & SIGNAL, "model supplies one accepted SIGNAL at a time"
        self.signal_kind = kind
        self.csr |= SIGNAL

    def finish(self):
        self.csr |= FINISH

    def begin_blank(self, gs_event=True, field_update=True):
        self.cp0 = (self.cp0 + (FRAME_TICKS - self.phase_ticks)
                    * EE_CYCLES_PER_TICK) & U32
        self.blank = True
        self.phase_ticks = 0
        if field_update:
            self.field ^= 1
        if gs_event:
            self.csr |= VSYNC

    def advance_phase(self, ticks):
        assert ticks >= self.phase_ticks
        self.cp0 = (self.cp0 + (ticks - self.phase_ticks)
                    * EE_CYCLES_PER_TICK) & U32
        self.phase_ticks = ticks
        self.blank = ticks < BLANK_TICKS

    def end_blank(self):
        self.advance_phase(max(BLANK_TICKS, self.phase_ticks))
        self.end_pending = True

    def service_end(self):
        # ROM may batch-ack end before it ever dispatches the pending GS IRQ.
        # Hardware phase, not this software/pending bit, must reject that edge.
        self.end_pending = False

    def request(self, intervals=1, has_buffers=True, expected_normal=None):
        assert self.requested == self.presented, "one pending descriptor only"
        assert self.cpu_front == self.front, "previous CPU rotation required"
        self.tokens["present"] = 0
        target = 1 - self.cpu_front
        self.descriptor = (target, target) if has_buffers else None
        # Snapshot publication occurs with interrupts disabled in the producer.
        self.aperture = None
        self.csr &= ~VSYNC
        self.intervals = intervals or 1
        self.expected_normal = (self.submitted if expected_normal is None
                                else expected_normal)
        self.requested = increment(self.requested)
        self.count("queued")
        return self.requested

    def begin_swap(self, intervals=1):
        self.cadence = intervals or 1
        if not self.async_enabled:
            assert self.finished(), "bootstrap has no in-flight normal renderer"
            self.async_enabled = True
            self.request(self.cadence)
        # Ordinary frame ends consume an existing request; they do not require
        # another boundary merely because the CPU reaches EndDrawing late.
        return self.requested

    def wait_for_presentation(self):
        self.count("joinWait" if self.requested != self.presented else "joinReady")
        if self.requested != self.presented:
            if self.tokens["present"]:
                self.tokens["present"] -= 1
            return False
        return True

    def reset_presentation(self):
        if not self.wait_for_presentation():
            return False
        self.async_enabled = False
        self.aperture = None
        self.field_observed = self.field_published = False
        self.repeat_history = self.repeat_count = self.history_fields = 0
        return True

    def standalone_wait(self):
        # A standalone wait is not a new descriptor's permission to publish.
        self.aperture = None
        self.csr &= ~VSYNC
        self.tokens["vsync"] = 0

    def try_present(self, at_vsync=False):
        if (self.presented == self.requested or self.intervals != 0
                or self.command_done != self.expected_normal
                or self.raster_done != self.expected_normal
                or self.aperture is None):
            return
        owner, saved_phase, stamp, field = self.aperture
        elapsed = (self.cp0 - stamp) & U32
        if owner != self.requested:
            return
        if (self.timer_mode != 0x9e or self.field != field
                or self.phase_ticks >= self.blank_ticks
                or self.phase_ticks < saved_phase
                or elapsed >= (self.blank_ticks - saved_phase)
                * EE_CYCLES_PER_TICK):
            self.count("expiredReady")
            self.aperture = None
            return
        if self.descriptor is not None:
            assert self.blank, "model published outside blank"
            self.front = self.descriptor[0]
            self.writes.append((self.requested, self.descriptor, self.field))
        self.published_field = self.field
        self.count("presented")
        if self.phase_ticks >= LEGACY_BLANK_TICKS:
            self.count("extendedBlank")
        if self.metrics_enabled:
            self.field_published = True
        self.count("atVsync" if at_vsync else "afterCompletion")
        self.presented = self.requested
        self.tokens["present"] += 1
        self.aperture = None

    def service_gs(self):
        # The real publication must complete within the admitted guard margin;
        # this model does not establish a bound in EE instructions or bus cycles.
        while self.csr & (SIGNAL | FINISH | VSYNC):
            csr = self.csr
            if csr & FINISH:
                self.csr &= ~FINISH
                if self.raster_done != self.submitted:
                    self.raster_done = self.submitted
                    self.tokens["raster"] += 1
            if csr & SIGNAL:
                if self.signal_kind == 1:
                    self.command_done = increment(self.command_done)
                    self.tokens["command"] += 1
                elif self.signal_kind == 2:
                    self.immediate_done = increment(self.immediate_done)
                self.csr &= ~SIGNAL
            if csr & VSYNC:
                if self.metrics_enabled and self.async_enabled:
                    if self.field_observed:
                        self.count("displayFields")
                        repeated = int(not self.field_published)
                        if repeated:
                            self.count("repeatedFields")
                        self.repeat_count -= (self.repeat_history >> 29) & 1
                        self.repeat_history = ((self.repeat_history << 1)
                                               | repeated) & 0x3fffffff
                        self.repeat_count += repeated
                        self.history_fields = min(self.history_fields + 1, 30)
                        if self.history_fields == 30:
                            self.count("windows30")
                            self.metrics["worstRepeat30"] = max(
                                self.metrics["worstRepeat30"], self.repeat_count)
                    self.field_observed = True
                    self.field_published = False
                    if self.presented == self.requested:
                        self.count("emptyQueueEdges")
                self.aperture = None
                if (self.csr & VSYNC and self.timer_mode == 0x9e
                        and self.phase_ticks < self.blank_ticks):
                    if self.presented != self.requested:
                        self.aperture = (self.requested, self.phase_ticks,
                                         self.cp0, self.field)
                        if self.intervals:
                            self.intervals -= 1
                        if (self.intervals == 0
                                and (self.command_done != self.expected_normal
                                     or self.raster_done != self.expected_normal)):
                            self.count("notReadyEdges")
                elif self.presented != self.requested:
                    self.count("lateEdges")
                self.tokens["vsync"] += 1
                self.csr &= ~VSYNC
            # A completion event may finish this request after its eligible
            # VSINT. The owned, live witness permits the same blank only.
            self.try_present(at_vsync=bool(csr & VSYNC))

    def resume(self, request):
        if self.presented != request:
            if self.tokens["present"]:
                self.tokens["present"] -= 1
            return False
        # Rotation/reclamation are CPU-only and follow the publication ack.
        if self.descriptor is not None:
            self.cpu_front = 1 - self.cpu_front
        self.rotations += 1
        return True


class EventOrderTests(unittest.TestCase):
    def completed_frame(self):
        model = Presentation()
        model.send_normal()
        model.signal()
        model.finish()
        model.service_gs()
        self.assertTrue(model.finished())
        return model

    def test_signal_and_finish_are_independent_in_both_orders(self):
        for first, second in (("signal", "finish"), ("finish", "signal")):
            with self.subTest(first=first):
                model = Presentation()
                self.assertEqual(model.send_normal(), (0x61, 0x60))
                getattr(model, first)()
                model.service_gs()
                self.assertFalse(model.finish_poll())
                request = model.request()
                model.begin_blank()
                model.service_gs()
                self.assertNotEqual(model.presented, request)
                getattr(model, second)()
                model.service_gs()
                self.assertTrue(model.finish_poll())
                self.assertEqual(model.presented, request)

    def test_stale_completion_tokens_cannot_retire_a_frame(self):
        model = Presentation()
        model.send_normal()
        model.tokens.update(command=3, raster=3)
        for _ in range(4):
            self.assertFalse(model.finish_poll())
        self.assertEqual(model.command_done, 0)
        self.assertEqual(model.raster_done, 0)
        model.signal()
        model.service_gs()
        for _ in range(4):
            self.assertFalse(model.finish_poll())
        model.finish()
        model.service_gs()
        self.assertTrue(model.finish_poll())

    def test_startup_and_repeated_finish_return_without_an_event(self):
        model = Presentation()
        for _ in range(3):
            self.assertTrue(model.finish_poll())
        model = self.completed_frame()
        model.tokens.update(command=0, raster=0)
        for _ in range(3):
            self.assertTrue(model.finish_poll())

    def test_immediate_chain_has_no_finish_or_normal_ack(self):
        model = Presentation()
        model.send_normal()
        self.assertEqual(model.send_immediate(), (0x60,))
        model.signal(2)
        model.service_gs()
        self.assertEqual(model.immediate_done, 1)
        self.assertEqual((model.command_done, model.raster_done), (0, 0))
        self.assertFalse(model.finished())

    def test_request_discards_serviced_and_unserviced_old_edges(self):
        for serviced in (False, True):
            with self.subTest(serviced=serviced):
                model = self.completed_frame()
                model.begin_blank()
                if serviced:
                    model.service_gs()
                model.tokens["present"] = 5
                request = model.request()
                model.service_gs()
                self.assertFalse(model.resume(request))
                self.assertEqual(model.writes, [])
                model.end_blank()
                model.service_end()
                model.begin_blank()
                model.service_gs()
                self.assertTrue(model.resume(request))

    def test_new_edge_after_request_can_ack_before_wait(self):
        model = self.completed_frame()
        request = model.request()
        model.begin_blank()
        model.service_gs()
        # Even losing all wake credit does not invalidate an observed ack.
        model.tokens["present"] = 0
        self.assertTrue(model.resume(request))
        self.assertEqual(len(model.writes), 1)

    def test_end_before_delayed_gs_dispatch_in_both_irq_orders(self):
        for end_first in (False, True):
            with self.subTest(end_first=end_first):
                model = self.completed_frame()
                request = model.request()
                model.begin_blank()
                model.end_blank()
                if end_first:
                    model.service_end()
                    model.service_gs()
                else:
                    model.service_gs()
                    model.service_end()
                self.assertFalse(model.resume(request))
                self.assertEqual(model.intervals, 1)
                self.assertEqual(model.writes, [])
                model.begin_blank()
                model.service_gs()
                self.assertTrue(model.resume(request))

    def test_multiple_intervals_count_only_eligible_edges(self):
        for intervals in (0, 1, 2, 4):
            with self.subTest(intervals=intervals):
                model = self.completed_frame()
                request = model.request(intervals)
                expected = intervals or 1
                for edge in range(expected):
                    model.begin_blank()
                    model.service_gs()
                    if edge + 1 < expected:
                        self.assertFalse(model.resume(request))
                        self.assertEqual(model.writes, [])
                    model.end_blank()
                    model.service_end()
                self.assertTrue(model.resume(request))
                self.assertEqual(len(model.writes), 1)

    def test_early_blank_cutoff_and_late_in_blank_dispatch(self):
        for phase_ticks in (0, 287, 288, 575, 576, 822, 823, 9000):
            with self.subTest(phase_ticks=phase_ticks):
                model = self.completed_frame()
                request = model.request()
                model.begin_blank()
                model.phase_ticks = phase_ticks
                model.blank = phase_ticks < BLANK_TICKS
                model.service_gs()
                eligible = phase_ticks < EARLY_BLANK_TICKS
                self.assertEqual(model.presented == request, eligible)
                self.assertEqual(len(model.writes), int(eligible))
                if not eligible:
                    self.assertEqual(model.intervals, 1)
                    model.begin_blank()
                    model.service_gs()
                    self.assertTrue(model.resume(request))

    def test_rom_batch_ack_cannot_make_expired_edge_eligible(self):
        model = self.completed_frame()
        request = model.request()
        model.begin_blank()
        model.end_blank()
        # Exactly the case where a test of INTC end-pending alone is unsafe.
        model.service_end()
        self.assertFalse(model.end_pending)
        self.assertTrue(model.csr & VSYNC)
        model.service_gs()
        self.assertFalse(model.resume(request))
        self.assertEqual(model.writes, [])

    def test_changed_timer_owner_does_not_admit_a_false_phase(self):
        model = self.completed_frame()
        request = model.request()
        model.begin_blank()
        model.timer_mode = 0x80
        model.service_gs()
        self.assertFalse(model.resume(request))
        self.assertEqual(model.intervals, 1)
        self.assertEqual(model.writes, [])

    def test_stale_present_token_never_releases_front_or_packet(self):
        model = self.completed_frame()
        request = model.request()
        model.tokens["present"] = 3
        for _ in range(4):
            self.assertFalse(model.resume(request))
        self.assertEqual((model.front, model.cpu_front, model.rotations), (0, 0, 0))
        with self.assertRaises(AssertionError):
            model.send_normal()
        model.begin_blank()
        model.service_gs()
        # IRQ changed the display, not the packet/CPU ownership.
        self.assertEqual((model.front, model.cpu_front, model.rotations), (1, 0, 0))
        with self.assertRaises(AssertionError):
            model.send_normal()
        self.assertTrue(model.resume(request))
        model.send_normal()

    def test_delayed_main_wake_cannot_move_the_flip(self):
        model = self.completed_frame()
        request = model.request()
        model.begin_blank()
        model.service_gs()
        publication = list(model.writes)
        field = model.published_field
        model.end_blank()
        model.service_end()
        model.begin_blank()
        model.service_gs()
        model.end_blank()
        model.service_end()
        self.assertTrue(model.resume(request))
        self.assertEqual(model.writes, publication)
        self.assertEqual(model.published_field, field)

    def test_uint32_wrap_uses_equality_not_ordering(self):
        model = Presentation()
        model.submitted = model.command_done = model.raster_done = U32
        model.requested = model.presented = U32
        model.send_normal()
        model.signal()
        model.finish()
        model.service_gs()
        self.assertEqual(model.submitted, 0)
        self.assertTrue(model.finished())
        request = model.request()
        self.assertEqual(request, 0)
        self.assertFalse(model.resume(request))
        model.begin_blank()
        model.service_gs()
        self.assertTrue(model.resume(request))

    def test_irq_drains_finish_signal_and_vsync_together(self):
        model = Presentation()
        model.send_normal()
        model.signal()
        model.finish()
        model.begin_blank()
        model.service_gs()
        self.assertEqual(model.csr & 11, 0)
        self.assertTrue(model.finished())
        self.assertEqual(model.tokens["vsync"], 1)

    def test_single_buffer_wait_ack_does_not_write_dispcircuit(self):
        model = Presentation()
        request = model.request(has_buffers=False)
        model.begin_blank()
        model.service_gs()
        self.assertEqual(model.presented, request)
        self.assertEqual(model.writes, [])
        self.assertTrue(model.resume(request))
        self.assertEqual(model.cpu_front, model.front)


class AsyncPipelineTests(unittest.TestCase):
    def running_frame(self, blank_ticks=EARLY_BLANK_TICKS):
        model = Presentation(blank_ticks)
        bootstrap = model.begin_swap()
        model.begin_blank()
        model.service_gs()
        self.assertTrue(model.resume(bootstrap))
        model.end_blank()
        model.service_end()
        model.send_normal()
        self.assertNotEqual(model.requested, model.presented)
        self.assertEqual(model.expected_normal, model.submitted)
        return model

    def test_previous_frame_presents_while_ee_prepares_next(self):
        model = self.running_frame()
        in_flight = model.requested
        model.signal()
        model.finish()
        model.service_gs()
        model.begin_blank()
        model.service_gs()
        self.assertEqual(model.presented, in_flight)
        self.assertNotEqual(model.cpu_front, model.front)
        # EE is still preparing its next packet for more than one interval.
        model.end_blank()
        model.service_end()
        model.begin_blank()
        model.service_gs()
        model.end_blank()
        model.service_end()
        writes = list(model.writes)
        # EndDrawing reuses the prior acknowledgement, without arming a dummy
        # new flip or waiting yet another display interval.
        self.assertEqual(model.begin_swap(), in_flight)
        self.assertTrue(model.resume(in_flight))
        self.assertEqual(model.writes, writes)
        model.send_normal()
        self.assertEqual(model.requested, increment(in_flight))

    def test_old_completion_cannot_publish_new_normal_owner(self):
        model = self.running_frame()
        model.begin_blank()
        model.service_gs()
        self.assertEqual(model.intervals, 0)
        self.assertFalse(model.wait_for_presentation())
        # A complete old frame is not the expected new normal sequence.
        self.assertEqual((model.command_done, model.raster_done), (0, 0))
        self.assertEqual(model.expected_normal, 1)
        self.assertEqual(len(model.writes), 1)  # bootstrap only

    def test_finish_after_boundary_uses_only_live_same_blank(self):
        for late_phase in (200, 287, 288, 575, 576, 9000):
            with self.subTest(late_phase=late_phase):
                model = self.running_frame()
                request = model.requested
                model.begin_blank()
                model.service_gs()
                self.assertEqual(model.intervals, 0)
                model.advance_phase(late_phase)
                model.signal()
                model.finish()
                model.service_gs()
                self.assertEqual(model.intervals, 0)
                if late_phase >= EARLY_BLANK_TICKS:
                    self.assertNotEqual(model.presented, request)
                    model.begin_blank()
                    model.service_gs()
                self.assertEqual(model.presented, request)

    def test_cadence_saturates_while_waiting_for_render_completion(self):
        model = self.running_frame()
        model.intervals = 2
        for expected in (1, 0, 0):
            model.begin_blank()
            model.service_gs()
            self.assertEqual(model.intervals, expected)
            self.assertFalse(model.wait_for_presentation())
            model.end_blank()
            model.service_end()
        model.signal()
        model.finish()
        model.service_gs()
        model.begin_blank()
        model.service_gs()
        self.assertTrue(model.wait_for_presentation())

    def test_wait_does_not_rotate_and_reset_requires_new_bootstrap(self):
        model = self.running_frame()
        request = model.requested
        rotations = model.rotations
        self.assertFalse(model.reset_presentation())
        self.assertTrue(model.async_enabled)
        model.signal()
        model.finish()
        model.service_gs()
        model.begin_blank()
        model.service_gs()
        self.assertTrue(model.wait_for_presentation())
        self.assertEqual(model.rotations, rotations)
        self.assertTrue(model.resume(request))
        self.assertTrue(model.reset_presentation())
        self.assertFalse(model.async_enabled)
        new_bootstrap = model.begin_swap()
        self.assertEqual(new_bootstrap, increment(request))
        self.assertFalse(model.wait_for_presentation())
        model.begin_blank()
        model.service_gs()
        self.assertTrue(model.resume(new_bootstrap))
        self.assertTrue(model.async_enabled)


class ApertureLifetimeTests(unittest.TestCase):
    def unfinished_at_edge(self, phase=0):
        model = AsyncPipelineTests().running_frame()
        model.begin_blank()
        model.advance_phase(phase)
        model.service_gs()
        self.assertIsNotNone(model.aperture)
        self.assertEqual(model.intervals, 0)
        return model

    def complete(self, model):
        model.signal()
        model.finish()
        model.service_gs()

    def test_next_request_cannot_reuse_previous_blank(self):
        model = self.unfinished_at_edge()
        self.complete(model)
        old_request = model.requested
        model.resume(old_request)
        model.send_normal()
        self.assertIsNone(model.aperture)
        model.advance_phase(200)
        self.complete(model)
        self.assertNotEqual(model.presented, model.requested)
        model.begin_blank()
        model.service_gs()
        self.assertEqual(model.presented, model.requested)

    def test_cp0_deadline_rejects_next_blank_before_gs_edge(self):
        for update_field in (False, True):
            with self.subTest(update_field=update_field):
                model = self.unfinished_at_edge()
                model.end_blank()
                # Timer1 can reset before a new GS VSINT/FIELD observation.
                # Even matching phase zero and stale FIELD cannot revive it.
                model.begin_blank(gs_event=False, field_update=update_field)
                self.complete(model)
                self.assertNotEqual(model.presented, model.requested)
                model.csr |= VSYNC
                model.service_gs()
                self.assertEqual(model.presented, model.requested)

    def test_cp0_deadline_boundary_and_counter_wrap(self):
        for wrap in (False, True):
            for offset in (-1, 0, 1):
                with self.subTest(wrap=wrap, offset=offset):
                    model = self.unfinished_at_edge(100)
                    owner, phase, _, field = model.aperture
                    stamp = U32 - 1000 if wrap else 1000
                    model.aperture = owner, phase, stamp, field
                    remaining = (EARLY_BLANK_TICKS - phase) * EE_CYCLES_PER_TICK
                    model.cp0 = (stamp + remaining + offset) & U32
                    # Deliberately hold Timer1 inside its accepted region to
                    # isolate the independent CP0 expiry check.
                    model.phase_ticks = 200
                    self.complete(model)
                    self.assertEqual(model.presented == model.requested, offset < 0)

    def test_phase_rewind_and_field_mismatch_reject_witness(self):
        for corrupt in ("phase", "field"):
            with self.subTest(corrupt=corrupt):
                model = self.unfinished_at_edge(100)
                if corrupt == "phase":
                    model.phase_ticks = 99
                else:
                    model.field ^= 1
                self.complete(model)
                self.assertNotEqual(model.presented, model.requested)

    def test_rejected_vsint_closes_prior_aperture(self):
        model = self.unfinished_at_edge()
        model.advance_phase(EARLY_BLANK_TICKS)
        model.csr |= VSYNC
        model.service_gs()
        self.assertIsNone(model.aperture)
        model.phase_ticks = 0
        self.complete(model)
        self.assertNotEqual(model.presented, model.requested)

    def test_standalone_wait_closes_prior_aperture(self):
        model = self.unfinished_at_edge()
        model.standalone_wait()
        self.assertIsNone(model.aperture)
        self.complete(model)
        self.assertNotEqual(model.presented, model.requested)
        model.begin_blank()
        model.service_gs()
        self.assertEqual(model.presented, model.requested)

    def test_wrapped_request_does_not_match_previous_owner(self):
        model = Presentation()
        model.requested = model.presented = U32
        model.request()
        model.begin_blank()
        model.intervals = 0
        model.aperture = (U32, 0, model.cp0, model.field)
        model.csr &= ~VSYNC
        model.finish()
        model.service_gs()
        self.assertEqual(model.requested, 0)
        self.assertEqual(model.presented, U32)
        model.csr |= VSYNC
        model.service_gs()
        self.assertEqual(model.presented, 0)

    def test_completion_cannot_bypass_second_interval(self):
        model = AsyncPipelineTests().running_frame()
        model.intervals = 2
        model.begin_blank()
        model.service_gs()
        self.assertEqual(model.intervals, 1)
        model.advance_phase(100)
        self.complete(model)
        self.assertNotEqual(model.presented, model.requested)
        model.begin_blank()
        model.service_gs()
        self.assertEqual(model.presented, model.requested)


class ApertureGateTests(unittest.TestCase):
    """Both limits expire permission; neither supplies raster completion."""

    limits = (LEGACY_BLANK_TICKS, EARLY_BLANK_TICKS)

    def unfinished(self, cutoff, phase=130):
        model = AsyncPipelineTests().running_frame(cutoff)
        model.set_metrics(True)
        model.begin_blank()
        model.advance_phase(phase)
        model.service_gs()
        self.assertIsNotNone(model.aperture)
        return model

    def complete(self, model):
        model.signal()
        model.finish()
        model.service_gs()

    def test_completion_in_extension_is_the_only_gate_difference(self):
        for cutoff in self.limits:
            for phase in (287, 288, 400, 575, 576, 823):
                for first, last in (("signal", "finish"), ("finish", "signal")):
                    with self.subTest(cutoff=cutoff, phase=phase, first=first):
                        model = self.unfinished(cutoff)
                        getattr(model, first)()
                        model.service_gs()
                        self.assertNotEqual(model.presented, model.requested)
                        model.advance_phase(phase)
                        getattr(model, last)()
                        model.service_gs()
                        self.assertEqual(model.presented == model.requested,
                                         phase < cutoff)
                        self.assertIsNone(model.aperture)
                        # This missed completion is invisible to lateEdges:
                        # the preceding VSINT was serviced promptly.
                        self.assertEqual(model.metrics["lateEdges"], 0)
                        self.assertEqual(model.metrics["notReadyEdges"], 1)
                        self.assertEqual(model.metrics["extendedBlank"],
                                         int(LEGACY_BLANK_TICKS <= phase < cutoff))
                        self.assertEqual(model.metrics["expiredReady"],
                                         int(phase >= cutoff))
                        # Expiring an aperture is a one-time classification,
                        # not repeated every time the helper is considered.
                        model.try_present()
                        self.assertEqual(model.metrics["expiredReady"],
                                         int(phase >= cutoff))

    def test_both_limits_reject_expired_count_witness_even_after_timer_reset(self):
        for cutoff in self.limits:
            for wrap in (False, True):
                for offset in (-1, 0, 1):
                    with self.subTest(cutoff=cutoff, wrap=wrap, offset=offset):
                        model = self.unfinished(cutoff)
                        owner, phase, _, field = model.aperture
                        stamp = U32 - 100 if wrap else 100
                        model.aperture = owner, phase, stamp, field
                        remaining = (cutoff - phase) * EE_CYCLES_PER_TICK
                        model.cp0 = (stamp + remaining + offset) & U32
                        model.phase_ticks = phase
                        self.complete(model)
                        self.assertEqual(model.presented == model.requested,
                                         offset < 0)
                        self.assertIsNone(model.aperture)
                        self.assertEqual(model.metrics["expiredReady"], int(offset >= 0))

    def test_both_limits_reject_next_blank_before_gs_field_changes(self):
        for cutoff in self.limits:
            for update_field in (False, True):
                with self.subTest(cutoff=cutoff, update_field=update_field):
                    model = self.unfinished(cutoff, phase=0)
                    model.end_blank()
                    model.begin_blank(gs_event=False, field_update=update_field)
                    self.complete(model)
                    self.assertNotEqual(model.presented, model.requested)
                    self.assertEqual(model.metrics["expiredReady"], 1)
                    self.assertIsNone(model.aperture)
                    model.csr |= VSYNC
                    model.service_gs()
                    self.assertEqual(model.presented, model.requested)

    def test_both_limits_reject_other_owner_field_timer_or_rewound_phase(self):
        for cutoff in self.limits:
            for invalid in ("owner", "field", "timer", "phase"):
                with self.subTest(cutoff=cutoff, invalid=invalid):
                    model = self.unfinished(cutoff)
                    owner, phase, stamp, field = model.aperture
                    if invalid == "owner":
                        model.aperture = increment(owner), phase, stamp, field
                    elif invalid == "field":
                        model.field ^= 1
                    elif invalid == "timer":
                        model.timer_mode = 0x80
                    else:
                        model.phase_ticks = phase - 1
                    self.complete(model)
                    self.assertNotEqual(model.presented, model.requested)
                    self.assertEqual(model.metrics["expiredReady"], int(invalid != "owner"))

    def test_both_limits_leave_margin_in_each_documented_timing_model(self):
        # Independent model values from the local hardware-tested PCSX2
        # timing implementation, NOT a measurement of Bruno's console or a
        # proof of MMIO bus latency. A 576-tick cutoff is 1 ms, not an entire
        # presumed blank; preserve the remaining 0.430 ms minimum margin.
        for cutoff in self.limits:
            for mode, blank_us in (("NTSC", 1430), ("PAL", 1568), ("480p", 1462)):
                with self.subTest(cutoff=cutoff, mode=mode):
                    cutoff_us = cutoff * 1_000_000 / 576_000
                    self.assertGreaterEqual(blank_us - cutoff_us, 430)


class DisplayIntervalMetricsTests(unittest.TestCase):
    def bootstrapped(self):
        model = Presentation()
        model.set_metrics(True)
        request = model.begin_swap()
        model.begin_blank()
        model.service_gs()
        self.assertTrue(model.resume(request))
        return model

    def unfinished(self):
        model = AsyncPipelineTests().running_frame()
        model.set_metrics(True)
        model.begin_blank()
        model.service_gs()
        return model

    def complete(self, model):
        model.advance_phase(200)
        model.signal()
        model.finish()
        model.service_gs()

    def next_edge(self, model):
        model.begin_blank()
        model.service_gs()

    def open_interval(self, model, published):
        if published:
            request = model.request()
        self.next_edge(model)
        if published:
            self.assertTrue(model.resume(request))

    def completed_pattern(self, pattern):
        self.assertTrue(pattern[0])
        model = self.bootstrapped()
        for published in pattern[1:]:
            self.open_interval(model, published)
        self.next_edge(model)
        return model

    def test_same_blank_rescue_is_not_a_repeated_display_interval(self):
        model = self.unfinished()
        self.assertEqual(model.metrics["notReadyEdges"], 1)
        self.assertEqual(model.metrics["displayFields"], 0)
        self.complete(model)
        self.assertEqual(model.metrics["afterCompletion"], 1)
        self.assertEqual(model.metrics["repeatedFields"], 0)
        self.next_edge(model)
        self.assertEqual(model.metrics["displayFields"], 1)
        self.assertEqual(model.metrics["repeatedFields"], 0)

    def test_cpu_no_request_edge_only_becomes_repeat_at_following_edge(self):
        model = self.bootstrapped()
        self.next_edge(model)
        # The empty queue belongs to the newly opened interval. The interval
        # being closed successfully published the bootstrap frame.
        self.assertEqual(model.metrics["emptyQueueEdges"], 1)
        self.assertEqual(model.metrics["displayFields"], 1)
        self.assertEqual(model.metrics["repeatedFields"], 0)
        self.next_edge(model)
        self.assertEqual(model.metrics["emptyQueueEdges"], 2)
        self.assertEqual(model.metrics["displayFields"], 2)
        self.assertEqual(model.metrics["repeatedFields"], 1)

    def test_gpu_not_ready_interval_repeats_even_with_nonempty_queue(self):
        model = self.unfinished()
        self.next_edge(model)
        self.assertEqual(model.metrics["displayFields"], 1)
        self.assertEqual(model.metrics["repeatedFields"], 1)
        self.assertEqual(model.metrics["emptyQueueEdges"], 0)
        self.assertEqual(model.metrics["notReadyEdges"], 2)
        self.complete(model)
        self.next_edge(model)
        self.assertEqual(model.metrics["displayFields"], 2)
        self.assertEqual(model.metrics["repeatedFields"], 1)

    def test_snapshot_retains_open_interval_before_and_after_rescue(self):
        for snapshot_before_completion in (True, False):
            with self.subTest(before=snapshot_before_completion):
                model = self.unfinished()
                if snapshot_before_completion:
                    snapshot = model.take_metrics()
                    self.complete(model)
                else:
                    self.complete(model)
                    snapshot = model.take_metrics()
                self.assertTrue(model.field_observed)
                self.assertTrue(model.field_published)
                self.assertEqual(snapshot["displayFields"], 0)
                self.next_edge(model)
                self.assertEqual(model.metrics["displayFields"], 1)
                self.assertEqual(model.metrics["repeatedFields"], 0)
                self.assertEqual(snapshot["presented"]
                                 + model.metrics["presented"], 1)

    def test_snapshot_retains_unpublished_interval_without_phantom_success(self):
        model = self.unfinished()
        snapshot = model.take_metrics()
        self.assertEqual(snapshot["notReadyEdges"], 1)
        self.assertTrue(model.field_observed)
        self.assertFalse(model.field_published)
        self.next_edge(model)
        self.assertEqual(model.metrics["displayFields"], 1)
        self.assertEqual(model.metrics["repeatedFields"], 1)

    def test_layout_reset_discards_open_interval_but_retains_closed_counts(self):
        model = self.bootstrapped()
        self.next_edge(model)
        self.assertTrue(model.reset_presentation())
        self.assertFalse(model.field_observed)
        self.assertFalse(model.field_published)
        self.assertEqual((model.repeat_history, model.repeat_count,
                          model.history_fields), (0, 0, 0))
        for _ in range(2):
            self.next_edge(model)
        self.assertEqual(model.metrics["displayFields"], 1)
        self.assertEqual(model.metrics["repeatedFields"], 0)
        model.begin_swap()
        self.next_edge(model)
        # No interval from the old layout is carried into the new bootstrap.
        self.assertEqual(model.metrics["displayFields"], 1)
        self.next_edge(model)
        self.assertEqual(model.metrics["displayFields"], 2)
        self.assertEqual(model.metrics["repeatedFields"], 0)

    def test_disabled_metrics_do_not_change_publication_or_retain_old_interval(self):
        writes = []
        for enabled in (False, True):
            model = self.unfinished()
            model.set_metrics(enabled)
            self.complete(model)
            self.next_edge(model)
            writes.append(model.writes)
            if not enabled:
                self.assertFalse(any(model.take_metrics().values()))
                self.assertFalse(model.field_observed)
                self.assertFalse(model.field_published)
            model.set_metrics(True)
            self.assertFalse(model.field_observed)
            self.assertFalse(model.field_published)
            self.assertEqual((model.repeat_history, model.repeat_count,
                              model.history_fields), (0, 0, 0))
            self.next_edge(model)
            self.assertEqual(model.metrics["displayFields"], 0)
            self.next_edge(model)
            self.assertEqual(model.metrics["displayFields"], 1)
            self.assertEqual(model.metrics["repeatedFields"], 1)
        self.assertEqual(writes[0], writes[1])

    def test_coalesced_vsints_are_observations_not_physical_refresh_counts(self):
        model = self.bootstrapped()
        model.begin_blank()
        model.begin_blank()  # CSR is a latched bit, not an edge counter.
        model.service_gs()
        self.assertEqual(model.metrics["displayFields"], 1)
        self.assertEqual(model.metrics["repeatedFields"], 0)
        self.assertEqual(model.metrics["emptyQueueEdges"], 1)

    def test_rolling_window_distinguishes_scattered_and_clustered_misses(self):
        for misses, peak in (((10, 30, 50), 2), ((20, 21, 22), 3)):
            with self.subTest(misses=misses):
                pattern = [index not in misses for index in range(60)]
                model = self.completed_pattern(pattern)
                self.assertEqual(model.metrics["displayFields"], 60)
                self.assertEqual(model.metrics["repeatedFields"], 3)
                self.assertEqual(model.metrics["windows30"], 31)
                self.assertEqual(model.metrics["worstRepeat30"], peak)
                self.assertEqual(model.repeat_count, sum(not x for x in pattern[-30:]))

    def test_rolling_window_warmup_exactly_thirty_observations(self):
        model = self.completed_pattern([True] * 29)
        self.assertEqual(model.history_fields, 29)
        self.assertEqual(model.metrics["windows30"], 0)
        # The helper's final edge left an unpublished interval open.
        self.next_edge(model)
        self.assertEqual(model.history_fields, 30)
        self.assertEqual(model.metrics["windows30"], 1)
        self.assertEqual(model.metrics["worstRepeat30"], 1)

    def test_rolling_window_preserves_history_across_report_snapshot(self):
        misses = (25, 26, 27, 45)
        pattern = [index not in misses for index in range(60)]
        model = self.bootstrapped()
        for published in pattern[1:40]:
            self.open_interval(model, published)
        # Thirty-nine intervals closed, with the fortieth still open. The
        # next report's first windows must retain earlier clustered repeats.
        snapshot = model.take_metrics()
        self.assertEqual(snapshot["windows30"], 10)
        self.assertEqual(snapshot["worstRepeat30"], 3)
        self.assertEqual(model.history_fields, 30)
        for published in pattern[40:]:
            self.open_interval(model, published)
        self.next_edge(model)
        self.assertEqual(model.metrics["windows30"], 21)
        self.assertEqual(model.metrics["worstRepeat30"], 4)
        self.assertEqual(snapshot["displayFields"] + model.metrics["displayFields"], 60)

    def test_rolling_history_resets_with_layout_or_metrics(self):
        for reset_kind in ("layout", "metrics"):
            with self.subTest(reset_kind=reset_kind):
                model = self.completed_pattern([True] + [False] * 34)
                self.assertEqual(model.metrics["worstRepeat30"], 30)
                if reset_kind == "layout":
                    self.assertTrue(model.reset_presentation())
                    # Accumulated report counters survive a layout reset.
                    self.assertEqual(model.metrics["worstRepeat30"], 30)
                    model.take_metrics()
                    model.begin_swap()
                    self.next_edge(model)
                else:
                    model.set_metrics(True)
                    self.next_edge(model)
                self.assertEqual((model.repeat_history, model.repeat_count,
                                  model.history_fields), (0, 0, 0))
                self.assertEqual(model.metrics["windows30"], 0)


def source_function(text, signature):
    """Find a simple C++ function body after stripping comments."""
    text = re.sub(r"/\*.*?\*/|//[^\n]*", "", text, flags=re.S)
    start = text.index(signature)
    start = text.index("{", start)
    depth = 1
    end = start + 1
    while depth:
        depth += (text[end] == "{") - (text[end] == "}")
        end += 1
    return text[start + 1:end - 1]


def source_with_metric_gates(text, frame=0, timeline=0):
    """Select known metrics branches, including nested gates; not a compiler."""
    values = dict(PGL_FRAME_PHASE_METRICS=frame, PGL_PRESENT_TIMELINE_METRICS=timeline)
    stack = []
    active = True
    lines = []
    for line in text.splitlines(keepends=True):
        match = re.match(r"^#if\s+(.+?)\s*$", line)
        if match:
            terms = match.group(1).split("&&")
            assert all(term.strip() in values for term in terms), match.group(1)
            condition = all(values[term.strip()] for term in terms)
            stack.append((active, condition))
            active = active and condition
        elif re.match(r"^#else\s*$", line):
            parent, condition = stack[-1]
            active = parent and not condition
        elif re.match(r"^#endif\s*$", line):
            active = stack.pop()[0]
        elif active:
            lines.append(line)
    assert not stack
    return "".join(lines)


def source_without_phase_metrics(text):
    return source_with_metric_gates(text)


class SourceContractTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.gl = (ROOT / "src/glcontext.cpp").read_text(encoding="utf-8")
        cls.display = (ROOT / "src/displaycontext.cpp").read_text(encoding="utf-8")

    def test_new_declarations_match_definitions(self):
        gl_h = (ROOT / "include/ps2gl/glcontext.h").read_text(encoding="utf-8")
        display_h = (ROOT / "include/ps2gl/displaycontext.h").read_text(encoding="utf-8")
        public_h = (ROOT / "include/GL/ps2gl.h").read_text(encoding="utf-8")
        self.assertIn("void SwapBuffersOnVSync(unsigned int intervals);", gl_h)
        self.assertIn("void SwapBuffers(bool displayAlreadyPresented = false);", gl_h)
        self.assertIn("void QueuePresentation(unsigned int intervals);", gl_h)
        self.assertIn("void WaitForPresentation();", gl_h)
        self.assertIn("void ResetPresentation();", gl_h)
        self.assertIn("static void TryPresent(bool atVsync);", gl_h)
        self.assertIn("bool PrepareBufferSwap(uint64_t* fb1, uint64_t* fb2);", display_h)
        self.assertIn("void SwapBuffers(bool displayAlreadyPresented = false);", display_h)
        self.assertIn("void pglSwapBuffersOnVSync(unsigned int intervals);", public_h)
        self.assertIn("void pglWaitForPresentation(void);", public_h)
        wait = source_function(self.gl, "void pglWaitForPresentation(void)")
        self.assertIn("pGLContext->WaitForPresentation();", wait)

    def test_terminal_packet_finishes_normal_only(self):
        body = source_function(self.gl, "void CGLContext::EndVif1Packet(")
        self.assertIn("tGifTag giftag = {};", body)
        self.assertIn("giftag.NLOOP = signalNum == 1 ? 2 : 1;", body)
        self.assertRegex(body, r"if\s*\(signalNum == 1\)\s*\{[^}]*0x61;")
        self.assertLess(body.index("0x61"), body.index("0x60"))
        self.assertLess(body.index("Vif1Packet->Flush();"), body.index("OpenDirect"))

    def test_all_enabled_irq_sources_are_drained(self):
        body = source_function(self.gl, "int CGLContext::GsIntHandler(")
        self.assertRegex(body, r"while\s*\(\(csr = .*?\) & 11\)")
        for event in (1, 2, 8):
            self.assertIn(f"if (csr & {event})", body)
        self.assertIn("GS::ControlRegs::imr = 0x7400;", self.gl)
        self.assertIn("NormalFramesFinished = NormalChainsSubmitted;", body)
        vsync = source_function(body, "if (csr & 8)")
        phase = source_function(vsync, "if (timerMode == PresentationTimerMode")
        self.assertIn("phase < PresentationBlankTicks", vsync)
        self.assertRegex(phase,
                         r"if\s*\(PresentationIntervals != 0\)\s*"
                         r"--PresentationIntervals;")
        self.assertEqual(body.count("--PresentationIntervals"), 1)
        self.assertIn("PresentationApertureRequest = PresentationRequested;", phase)
        self.assertIn("PresentationApertureOpen = true;", phase)
        self.assertLess(vsync.index("PresentationApertureOpen = false;"),
                        vsync.index("if (PresentationCompleted != PresentationRequested)"))
        self.assertLess(body.index("csr = 8;"), body.index("TryPresent((csr & 8) != 0);"))
        # Completion IRQs may consume a witnessed aperture, but never advance
        # cadence independently of VSINT or acknowledge publication directly.
        for event in (1, 2):
            other = source_function(body, f"if (csr & {event})")
            self.assertNotIn("PresentationIntervals", other)
            self.assertNotIn("PresentationCompleted =", other)
        self.assertIn("PresentationTimerMode = 0x9e;", self.gl)
        self.assertIn("PresentationEarlyBlankTicks = 288;", self.gl)
        self.assertIn("PresentationBlankTicks = PGL_PRESENT_EXTENDED_BLANK ? 576 : 288;", self.gl)
        self.assertNotIn("IntcStatus", body)

    def test_extended_blank_gate_is_independent_of_diagnostics(self):
        public_h = (ROOT / "include/GL/ps2gl.h").read_text(encoding="utf-8")
        self.assertIn("#ifndef PGL_PRESENT_EXTENDED_BLANK", public_h)
        self.assertIn("#define PGL_PRESENT_EXTENDED_BLANK 1", public_h)
        self.assertIn("#if PGL_PRESENT_EXTENDED_BLANK != 0 && PGL_PRESENT_EXTENDED_BLANK != 1",
                      public_h)
        disabled = source_without_phase_metrics(self.gl)
        self.assertIn("PresentationBlankTicks = PGL_PRESENT_EXTENDED_BLANK ? 576 : 288;",
                      disabled)
        body = source_function(self.gl, "void CGLContext::TryPresent(")
        expired = body.index("PGL_PRESENT_COUNT(expiredReady);")
        self.assertLess(body.index("NormalFramesFinished != PresentationNormalSequence"), expired)
        self.assertLess(body.index("phase >= PresentationBlankTicks"), expired)
        self.assertLess(expired, body.index("PresentationApertureOpen = false;"))
        self.assertLess(body.index("dispfb2 = PresentationFB2"),
                        body.index("PGL_PRESENT_COUNT(extendedBlank);"))
        self.assertRegex(body, r"if\s*\(phase >= PresentationEarlyBlankTicks\)\s*"
                              r"PGL_PRESENT_COUNT\(extendedBlank\);")
        snapshot = source_function(self.gl, 'extern "C" GLboolean pglTakePresentationMetrics(')
        self.assertIn("stats->blankTicks = PresentationBlankTicks;", snapshot)
        for field in ("blankTicks", "extendedBlank", "expiredReady"):
            self.assertRegex(public_h, rf"unsigned int [^;]*\b{field}\b")

    def test_timer_reservation_precedes_request_and_rejects_owner(self):
        body = source_function(self.gl, "void CGLContext::QueuePresentation(")
        self.assertIn("if (SavedPresentationTimerMode & 0x80)", body)
        self.assertIn("*R_EE_T1_COUNT = 0xffff;", body)
        self.assertLess(body.index("*R_EE_T1_MODE = PresentationTimerMode | 0xc00;"),
                        body.index("PresentationRequested = request;"))
        destructor = source_function(self.gl, "CGLContext::~CGLContext()")
        self.assertIn("*R_EE_T1_COUNT = SavedPresentationTimerCount;", destructor)
        self.assertIn("*R_EE_T1_MODE = SavedPresentationTimerMode;", destructor)

    def test_completion_wait_is_repeatable_and_does_not_trust_tokens(self):
        body = source_function(self.gl, "void CGLContext::FinishRenderingGeometry(")
        self.assertIn("while (NormalChainsCompleted != submitted)", body)
        self.assertIn("while (NormalFramesFinished != submitted)", body)
        send = source_function(self.gl, "void CGLContext::RenderGeometry()")
        self.assertIn("NormalFramesFinished != NormalChainsSubmitted", send)
        self.assertIn("PresentationCompleted != PresentationRequested", send)
        self.assertLess(send.index("csr = 2"), send.index("++NormalChainsSubmitted"))
        self.assertLess(send.index("++NormalChainsSubmitted"), send.index("QueuePresentation("))
        self.assertLess(send.index("QueuePresentation("), send.index("LastPacket->Send"))
        self.assertRegex(send,
                         r"if\s*\(PresentationEnabled\)\s*"
                         r"QueuePresentation\(PresentationCadence\);")

    def test_publication_precedes_cpu_rotation(self):
        queue = source_function(self.gl, "void CGLContext::QueuePresentation(")
        request = queue.index("PresentationRequested = request;")
        self.assertLess(queue.index("PrepareBufferSwap("), request)
        self.assertLess(queue.index("DIntr()"), queue.index("csr = 8"))
        self.assertLess(queue.index("csr = 8"), request)
        self.assertLess(queue.index("PresentationNormalSequence = NormalChainsSubmitted"), request)
        self.assertIn("if (interruptsEnabled) EIntr();", queue[request:])
        body = source_function(self.gl, "void CGLContext::SwapBuffersOnVSync(")
        bootstrap = source_function(body, "if (!PresentationEnabled)")
        self.assertIn("QueuePresentation(PresentationCadence);", bootstrap)
        self.assertEqual(body.count("QueuePresentation("), 1)
        self.assertLess(body.index("WaitForPresentation();"), body.index("SwapBuffers(true)"))
        self.assertLess(body.index("IsCurrentFieldEven = PresentationFieldIsEven"),
                        body.index("SwapBuffers(true)"))
        wait = source_function(self.gl, "void CGLContext::WaitForPresentation()")
        self.assertIn("while (PresentationCompleted != request)", wait)
        self.assertIn("WaitSema(PresentationSemaId);", wait)
        self.assertNotIn("SwapBuffers(", wait)
        self.assertNotIn("QueuePresentation(", wait)
        irq = source_function(self.gl, "int CGLContext::GsIntHandler(")
        publish = source_function(self.gl, "void CGLContext::TryPresent(")
        self.assertLess(publish.index("dispfb2 = PresentationFB2"),
                        publish.index("PresentationCompleted = PresentationRequested"))
        for forbidden in ("SwapBuffers(", "FreeWaitingBuffers", "malloc(", "free("):
            self.assertNotIn(forbidden, irq)
            self.assertNotIn(forbidden, publish)
        display = source_function(self.display, "void CDisplayContext::SwapBuffers(")
        self.assertLess(display.index("if (displayAlreadyPresented) return;"),
                        display.index("SendFBFlip()"))

    def test_game_uses_wait_and_publish_wrapper_glut_retains_timer_owner(self):
        raylib = (ROOT.parent / "raylib-ps2/src/platforms/rcore_playstation2.c").read_text(encoding="utf-8")
        swap = source_function(raylib, "void SwapScreenBuffer(void)")
        self.assertIn("pglSwapBuffersOnVSync((unsigned int)gVBlankDivisor);", swap)
        self.assertNotIn("pglWaitForVSync", swap)
        self.assertNotIn("pglSwapBuffers();", swap)
        self.assertLess(swap.index("pglSwapBuffersOnVSync"), swap.index("pglRenderGeometry"))
        glut = (ROOT / "glut/src/ps2glut.cpp").read_text(encoding="utf-8")
        self.assertNotIn("pglSwapBuffersOnVSync", glut)
        self.assertIn("pglWaitForVSync();", glut)

    def test_layout_reset_and_mode_changes_retire_immutable_descriptor(self):
        reset = source_function(self.gl, "void CGLContext::ResetPresentation()")
        self.assertLess(reset.index("WaitForPresentation();"),
                        reset.index("PresentationEnabled = false;"))
        self.assertNotIn("SwapBuffers(", reset)
        buffers = source_function(self.display, "void CDisplayContext::SetDisplayBuffers(")
        self.assertLess(buffers.index("GLContext.ResetPresentation();"),
                        buffers.index("Frame0Mem = frame0Mem;"))
        mode = source_function(self.display, "void CDisplayContext::SetVideoMode(")
        self.assertLess(mode.index("GLContext.WaitForPresentation();"),
                        mode.index("DisplayEnv->SetDisplayMode("))
        self.assertIn("DisplayEnv->SetFB2(DisplayEnv->GetFB2Addr(),", mode)
        self.assertNotIn("ResetPresentation", mode)
        flicker = source_function(self.display, "void CDisplayContext::ApplyFlicker(")
        self.assertIn("DisplayEnv->SetFB1(DisplayEnv->GetFB2Addr(),", flicker)
        display_h = (ROOT.parent / "ps2stuff/include/ps2s/displayenv.h").read_text(encoding="utf-8")
        self.assertIn("GetFB2Addr() const { return gsrDispFB2.FBP * 2048; }", display_h)
        game = (ROOT.parents[2] / "playstation2.c").read_text(encoding="utf-8")
        before_free = game[:game.index("pglDestroyGsMemArea(g_gs_f0)")]
        self.assertIn("pglWaitForPresentation();", before_free)

    def test_same_blank_helper_requires_owned_live_aperture(self):
        body = source_function(self.gl, "void CGLContext::TryPresent(")
        for condition in (
                "PresentationCompleted == PresentationRequested",
                "!PresentationApertureOpen",
                "PresentationApertureRequest != PresentationRequested",
                "PresentationIntervals != 0",
                "NormalChainsCompleted != PresentationNormalSequence",
                "NormalFramesFinished != PresentationNormalSequence",
                "phase < PresentationAperturePhase",
                "phase >= PresentationBlankTicks",
                "elapsed >= (PresentationBlankTicks - PresentationAperturePhase) * 512u",
                "fieldIsEven != PresentationApertureFieldIsEven"):
            self.assertIn(condition, body)
        self.assertIn("clock = ReadEeCycleCount();", body)
        self.assertIn("elapsed = clock - PresentationApertureClock", body)
        self.assertIn("PresentationApertureOpen = false;", body)
        self.assertIn("volatile uint32_t*)GS::ControlRegs::csr", body)
        irq = source_function(self.gl, "int CGLContext::GsIntHandler(")
        vsync = source_function(irq, "if (csr & 8)")
        self.assertLess(vsync.index("apertureClock = ReadEeCycleCount();"),
                        vsync.index("phase = *R_EE_T1_COUNT;"))
        queue = source_function(self.gl, "void CGLContext::QueuePresentation(")
        self.assertLess(queue.index("PresentationApertureOpen = false;"),
                        queue.index("PresentationRequested = request;"))
        for signature in ("void CGLContext::WaitForVSync()",
                          "void CGLContext::ResetPresentation()"):
            invalidate = source_function(self.gl, signature)
            self.assertLess(invalidate.index("DIntr()"),
                            invalidate.index("PresentationApertureOpen = false;"))

    def test_presentation_remains_functional_with_metrics_compiled_out(self):
        disabled = source_without_phase_metrics(self.gl)
        self.assertNotIn("#if PGL_FRAME_PHASE_METRICS", disabled)
        self.assertNotIn("FramePhaseEnabled", disabled)
        for diagnostic in ("PresentationFieldObserved", "PresentationFieldPublished",
                           "PresentationRepeatHistory", "PresentationRepeatCount",
                           "PresentationHistoryFields"):
            self.assertNotIn(diagnostic, disabled)
        clock = source_function(disabled, "static inline unsigned int ReadEeCycleCount()")
        self.assertIn("mfc0 %0, $9", clock)
        helper = source_function(disabled, "void CGLContext::TryPresent(")
        self.assertIn("clock = ReadEeCycleCount();", helper)
        self.assertIn("elapsed = clock - PresentationApertureClock", helper)
        self.assertIn("dispfb2 = PresentationFB2", helper)
        irq = source_function(disabled, "int CGLContext::GsIntHandler(")
        self.assertIn("TryPresent((csr & 8) != 0);", irq)
        self.assertIn("#define PGL_PRESENT_COUNT(field) ((void)0)", disabled)
        stats = source_function(disabled, 'extern "C" GLboolean pglTakePresentationMetrics(')
        self.assertIn("memset(stats, 0, sizeof(*stats));", stats)
        self.assertIn("return GL_FALSE;", stats)

    def test_interval_metrics_close_after_rescue_and_preserve_snapshot_history(self):
        irq = source_function(self.gl, "int CGLContext::GsIntHandler(")
        vsync = source_function(irq, "if (csr & 8)")
        ledger = source_function(vsync, "if (FramePhaseEnabled && PresentationEnabled)")
        self.assertLess(ledger.index("++PresentationStats.displayFields;"),
                        ledger.index("PresentationFieldPublished = false;"))
        self.assertIn("PresentationFieldPublished ? 0u : 1u", ledger)
        self.assertIn("if (PresentationCompleted == PresentationRequested)", ledger)
        self.assertIn("++PresentationStats.emptyQueueEdges;", ledger)
        self.assertIn("if (PresentationHistoryFields == 30)", ledger)
        self.assertIn("++PresentationStats.windows30;", ledger)
        publish = source_function(self.gl, "void CGLContext::TryPresent(")
        self.assertLess(publish.index("dispfb2 = PresentationFB2"),
                        publish.index("PresentationFieldPublished = true;"))
        snapshot = source_function(self.gl, 'extern "C" GLboolean pglTakePresentationMetrics(')
        self.assertLess(snapshot.index("DIntr()"), snapshot.index("memcpy(stats,"))
        self.assertLess(snapshot.index("memset(&PresentationStats"), snapshot.index("EIntr()"))
        for name in ("PresentationFieldObserved", "PresentationFieldPublished",
                     "PresentationRepeatHistory", "PresentationRepeatCount",
                     "PresentationHistoryFields"):
            self.assertNotIn(name, snapshot)
        for signature in ('extern "C" GLboolean pglSetFramePhaseMetrics(',
                          "CGLContext::CGLContext(",
                          "void CGLContext::ResetPresentation()"):
            reset = source_function(self.gl, signature)
            self.assertIn("PresentationFieldObserved = PresentationFieldPublished = false;", reset)
            self.assertIn("PresentationRepeatHistory = PresentationRepeatCount = PresentationHistoryFields = 0;",
                          reset)
        public_h = (ROOT / "include/GL/ps2gl.h").read_text(encoding="utf-8")
        for field in ("displayFields", "repeatedFields", "emptyQueueEdges",
                      "windows30", "worstRepeat30"):
            self.assertRegex(public_h, rf"unsigned int [^;]*\b{field}\b")


if __name__ == "__main__":
    print("Host model only: not an EE/kernel/GS execution or blank-duration proof.")
    unittest.main(verbosity=2)

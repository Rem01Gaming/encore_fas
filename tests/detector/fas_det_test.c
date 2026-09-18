// SPDX-License-Identifier: GPL-2.0-only

#include <math.h>
#include <stdio.h>
#include <stdlib.h>

#include "encore_fas_det.h"

#define MAXF 200000
#define MAXE 100000

static u64 rng_state = 88172645463325252ULL;
static bool verbose;
static int failures;

static double urand(void)
{
	rng_state ^= rng_state << 13;
	rng_state ^= rng_state >> 7;
	rng_state ^= rng_state << 17;
	return ((rng_state >> 11) + 0.5) / 9007199254740992.0;
}

static double gauss(void)
{
	return sqrt(-2.0 * log(urand())) * cos(6.283185307179586 * urand());
}

struct trace {
	u64 ns[MAXF];
	int n;
	double nominal;
};

struct ev {
	u64 t;
	u32 type;
	u32 flags;
	u32 missed;
	u32 fps;
};

struct run {
	struct fas_cfg c;
	struct fas_hot h;
	u64 freq;
	u64 wd_at;
	struct ev e[MAXE];
	int n;
};

static void tpush(struct trace *tr, double t_ns)
{
	u64 t = (u64)(t_ns < 0 ? 0 : t_ns);

	if (tr->n && t <= tr->ns[tr->n - 1] + 300000ULL)
		t = tr->ns[tr->n - 1] + 300000ULL;
	tr->ns[tr->n++] = t;
}

/* Adds a segment. The period moves linearly from p0 to p1, both in ms. */
static void seg(struct trace *tr, double dur_s, double p0, double p1, double sig_ms)
{
	double el = 0, total = dur_s * 1000.0;

	while (el < total) {
		double p = p0 + (p1 - p0) * (el / total);

		tr->nominal += p * 1e6;
		tpush(tr, tr->nominal + gauss() * sig_ms * 1e6);
		el += p;
	}
}

static u64 to_ticks(const struct run *r, u64 ns)
{
	return (u64)((double)ns * (double)r->freq / 1e9);
}

static void log_out(struct run *r, u64 t, const struct fas_out *o)
{
	u32 i;

	for (i = 0; i < o->n && r->n < MAXE; i++) {
		struct ev *e = &r->e[r->n++];

		e->t = t;
		e->type = o->ev[i].type;
		e->flags = o->ev[i].flags;
		e->missed = o->ev[i].missed;
		e->fps = o->ev[i].fps;
	}
}

static void fire_watchdog(struct run *r, u64 until)
{
	while (r->wd_at && r->wd_at <= until) {
		struct fas_out o;
		u64 at = r->wd_at;
		u64 d = fas_det_wd_fire(&r->h, &r->c, at, &o);

		log_out(r, at, &o);
		r->wd_at = d ? at + d : 0;
	}
}

static int run_trace(struct run *r, const struct trace *tr, u64 freq,
		     const u32 *fps, u32 count, double vsync_ms, bool lock,
		     double tail_s)
{
	int i;

	r->freq = freq;
	r->n = 0;
	r->wd_at = 0;
	if (fas_det_setup(&r->c, &r->h, freq, fps, count,
			  (u64)(vsync_ms * 1e-3 * (double)freq), lock)) {
		printf("setup failed\n");
		exit(2);
	}

	for (i = 0; i < tr->n; i++) {
		struct fas_out o;
		u64 now = to_ticks(r, tr->ns[i]);
		u64 d;

		fire_watchdog(r, now);
		d = fas_det_frame(&r->h, &r->c, now, &o);
		log_out(r, now, &o);
		if (d)
			r->wd_at = now + d;
	}
	if (tail_s > 0)
		fire_watchdog(r, to_ticks(r, tr->ns[tr->n - 1]) +
					 (u64)(tail_s * (double)freq));
	return r->n;
}

static int count(const struct run *r, u32 type, u64 from_ns, u64 to_ns)
{
	int i, c = 0;
	u64 from = (u64)((double)from_ns * (double)r->freq / 1e9);
	u64 to = to_ns == ~0ULL ? ~0ULL :
				  (u64)((double)to_ns * (double)r->freq / 1e9);

	for (i = 0; i < r->n; i++)
		if (r->e[i].type == type && r->e[i].t >= from && r->e[i].t < to)
			c++;
	return c;
}

/* Returns the delay from @from_ns to the first event of @type, in ms. */
static double first_ms(const struct run *r, u32 type, u64 from_ns)
{
	int i;
	u64 from = (u64)((double)from_ns * (double)r->freq / 1e9);

	for (i = 0; i < r->n; i++)
		if (r->e[i].type == type && r->e[i].t >= from)
			return (double)(r->e[i].t - from) * 1e3 / (double)r->freq;
	return -1;
}

#define CHECK(cond, ...)                                                   \
	do {                                                               \
		if (!(cond)) {                                             \
			failures++;                                        \
			printf("FAIL [%s:%d] freq=%llu: ", __func__, __LINE__, \
			       (unsigned long long)freq);                   \
			printf(__VA_ARGS__);                               \
			printf("\n");                                      \
		}                                                          \
	} while (0)

static struct trace TR;
static struct run RN;

static void t_reciprocal(u64 freq)
{
	u32 rates[] = { 24, 30, 45, 60, 90, 120, 144, 240 };
	unsigned i;
	long worst = 0;

	for (i = 0; i < sizeof(rates) / sizeof(rates[0]); i++) {
		struct fas_cfg c;
		struct fas_hot h;
		u32 f = rates[i];
		u64 p, d;

		if (fas_det_setup(&c, &h, freq, &f, 1, 0, false))
			exit(2);
		p = c.period;
		for (d = 0; d <= p + (p >> 1); d += p / 97 + 1) {
			s64 exact = ((s64)d - (s64)p) * FAS_Q_ONE / (s64)p;
			s64 fast = ((s64)d - (s64)p) * (s64)c.recip >>
				   (FAS_RECIP_SHIFT - FAS_Q);
			long e = labs((long)(fast - exact));

			if (e > worst)
				worst = e;
		}
	}
	if (verbose)
		printf("reciprocal error: worst %ld LSB of Q16\n", worst);
	CHECK(worst <= 2, "reciprocal error %ld LSB", worst);
}

static void t_healthy(u64 freq)
{
	struct { double sig, vsync; } c[] = { { 0.3, 16.667 }, { 1.0, 8.333 },
					      { 2.0, 8.333 } };
	u32 fps[] = { 60 };
	unsigned i;

	for (i = 0; i < 3; i++) {
		int bad;

		rng_state = 1000 + i;
		TR.n = 0;
		TR.nominal = 0;
		seg(&TR, 900, 1000.0 / 60, 1000.0 / 60, c[i].sig);
		run_trace(&RN, &TR, freq, fps, 1, c[i].vsync, false, 0);
		bad = count(&RN, FAS_EVENT_SMALL_JANK, 0, ~0ULL) +
		      count(&RN, FAS_EVENT_BIG_JANK, 0, ~0ULL) +
		      count(&RN, FAS_EVENT_DEGRADED, 0, ~0ULL) +
		      count(&RN, FAS_EVENT_BOOST_SOFT, 0, ~0ULL);
		if (verbose)
			printf("healthy sigma=%.1fms vsync=%.2fms 15min: %d false events\n",
			       c[i].sig, c[i].vsync, bad);
		CHECK(bad <= 3, "false events %d", bad);
		CHECK(count(&RN, FAS_EVENT_DEGRADED, 0, ~0ULL) == 0, "false degraded");
	}
}

static void t_hitches(u64 freq)
{
	double delays[] = { 16.667, 33.333, 100.0 };
	u32 fps[] = { 60 };
	int j;

	for (j = 0; j < 3; j++) {
		int h, small, big, soft, wdflag = 0, i;

		rng_state = 2000 + j;
		TR.n = 0;
		TR.nominal = 0;
		seg(&TR, 2, 1000.0 / 60, 1000.0 / 60, 0.3);
		for (h = 0; h < 200; h++)
			for (i = 0; i < 120; i++) {
				double t;

				TR.nominal += 1000.0 / 60 * 1e6;
				t = TR.nominal + gauss() * 0.3e6;
				if (i == 60)
					t += delays[j] * 1e6;
				tpush(&TR, t);
			}
		run_trace(&RN, &TR, freq, fps, 1, 16.667, false, 0);
		small = count(&RN, FAS_EVENT_SMALL_JANK, 0, ~0ULL);
		big = count(&RN, FAS_EVENT_BIG_JANK, 0, ~0ULL);
		soft = count(&RN, FAS_EVENT_BOOST_SOFT, 0, ~0ULL);
		for (i = 0; i < RN.n; i++)
			if ((RN.e[i].type == FAS_EVENT_SMALL_JANK ||
			     RN.e[i].type == FAS_EVENT_BIG_JANK) &&
			    (RN.e[i].flags & FAS_EVF_WATCHDOG))
				wdflag++;
		if (verbose)
			printf("hitch +%.1fms x200: small=%d big=%d soft=%d wdflag=%d degraded=%d\n",
			       delays[j], small, big, soft, wdflag,
			       count(&RN, FAS_EVENT_DEGRADED, 0, ~0ULL));
		CHECK(small + big == 200, "hitch events %d", small + big);
		CHECK(soft == 200, "soft events %d", soft);
		CHECK(wdflag == 200, "watchdog flag %d", wdflag);
		if (j == 2)
			CHECK(big == 200, "big hitches %d", big);
		CHECK(count(&RN, FAS_EVENT_DEGRADED, 0, ~0ULL) == 0, "burst hitch degraded");
	}
}

static void t_deficit(u64 freq)
{
	double fx[] = { 55, 50, 45, 30 };
	double maxms[] = { 700, 250, 150, 200 };
	u32 fps[] = { 60 };
	int j;

	for (j = 0; j < 4; j++) {
		u64 onset;
		double lat;

		rng_state = 3000 + j;
		TR.n = 0;
		TR.nominal = 0;
		seg(&TR, 5, 1000.0 / 60, 1000.0 / 60, 0.3);
		onset = TR.ns[TR.n - 1];
		seg(&TR, 10, 1000.0 / fx[j], 1000.0 / fx[j], 0.3);
		run_trace(&RN, &TR, freq, fps, 1, 16.667, false, 0);
		lat = first_ms(&RN, FAS_EVENT_DEGRADED, onset);
		if (verbose)
			printf("deficit %2.0f fps (%.1f%%): degraded after %.0f ms\n", fx[j],
			       (60.0 / fx[j] - 1) * 100, lat);
		CHECK(lat >= 0 && lat <= maxms[j], "latency %.0f ms", lat);
	}

	rng_state = 3100;
	TR.n = 0;
	TR.nominal = 0;
	seg(&TR, 60, 1000.0 / 60, 1000.0 / 60 * 1.03, 0.3);
	run_trace(&RN, &TR, freq, fps, 1, 16.667, false, 0);
	CHECK(count(&RN, FAS_EVENT_DEGRADED, 0, ~0ULL) == 0,
	      "a 3 percent deficit must stay inside the tolerance");

	{
		u64 onset;
		double p;

		rng_state = 3200;
		TR.n = 0;
		TR.nominal = 0;
		seg(&TR, 5, 1000.0 / 60, 1000.0 / 60, 0.3);
		onset = TR.ns[TR.n - 1];
		seg(&TR, 30, 1000.0 / 60, 2000.0 / 60, 0.3);
		run_trace(&RN, &TR, freq, fps, 1, 16.667, false, 0);
		p = 1000.0 / 60 + (1000.0 / 60) * first_ms(&RN, FAS_EVENT_DEGRADED, onset) / 30000.0;
		if (verbose)
			printf("ramp 60 to 30 fps: degraded at period %.1f ms\n", p);
		CHECK(first_ms(&RN, FAS_EVENT_DEGRADED, onset) > 0 && p < 1000.0 / 60 * 1.15,
		      "ramp degraded at period %.1f", p);
	}
}

static void t_rates(u64 freq)
{
	u32 fps[] = { 30, 60, 120 };
	u64 onset;
	double up, down, dg;

	rng_state = 4000;
	TR.n = 0;
	TR.nominal = 0;
	seg(&TR, 3, 1000.0 / 120, 1000.0 / 120, 0.2);
	onset = TR.ns[TR.n - 1];
	seg(&TR, 10, 1000.0 / 60, 1000.0 / 60, 0.2);
	run_trace(&RN, &TR, freq, fps, 3, 8.333, false, 0);
	down = first_ms(&RN, FAS_EVENT_RATE_SWITCH, onset);
	dg = first_ms(&RN, FAS_EVENT_DEGRADED, onset);
	if (verbose)
		printf("120->60: degraded +%.0f ms, switch +%.0f ms\n", dg, down);
	CHECK(dg > 0 && dg < 200, "degraded %.0f", dg);
	CHECK(down > 900 && down < 1400, "switch down %.0f", down);

	run_trace(&RN, &TR, freq, fps, 3, 8.333, true, 0);
	CHECK(count(&RN, FAS_EVENT_RATE_SWITCH, onset, ~0ULL) == 0, "lock switched");
	CHECK(count(&RN, FAS_EVENT_DEGRADED, onset, ~0ULL) == 1, "lock degraded");

	rng_state = 4001;
	TR.n = 0;
	TR.nominal = 0;
	seg(&TR, 3, 1000.0 / 60, 1000.0 / 60, 0.2);
	onset = TR.ns[TR.n - 1];
	seg(&TR, 10, 1000.0 / 120, 1000.0 / 120, 0.2);
	run_trace(&RN, &TR, freq, fps, 3, 8.333, false, 0);
	up = first_ms(&RN, FAS_EVENT_RATE_SWITCH, onset);
	if (verbose)
		printf("60->120: switch +%.0f ms\n", up);
	CHECK(up > 400 && up < 900, "switch up %.0f", up);
	CHECK(count(&RN, FAS_EVENT_DEGRADED, 0, ~0ULL) == 0, "up degraded");
}

static void t_pause_recover(u64 freq)
{
	u32 fps[] = { 60 };
	u64 last;
	double soft, hard, paused, resumed;

	rng_state = 5000;
	TR.n = 0;
	TR.nominal = 0;
	seg(&TR, 3, 1000.0 / 60, 1000.0 / 60, 0.3);
	last = TR.ns[TR.n - 1];
	TR.nominal += 3000e6;
	seg(&TR, 3, 1000.0 / 60, 1000.0 / 60, 0.3);
	run_trace(&RN, &TR, freq, fps, 1, 16.667, false, 0);
	soft = first_ms(&RN, FAS_EVENT_BOOST_SOFT, last);
	hard = first_ms(&RN, FAS_EVENT_BOOST_HARD, last);
	paused = first_ms(&RN, FAS_EVENT_PAUSED, last);
	resumed = first_ms(&RN, FAS_EVENT_RESUMED, last);
	if (verbose)
		printf("pause: soft +%.1f hard +%.1f paused +%.1f resumed +%.1f ms\n", soft,
		       hard, paused, resumed);
	CHECK(soft > 24 && soft < 26, "soft %.1f", soft);
	CHECK(hard > 57 && hard < 59, "hard %.1f", hard);
	CHECK(paused > 999 && paused < 1001, "paused %.1f", paused);
	CHECK(resumed > 3000 && resumed < 3040, "resumed %.1f", resumed);
	CHECK(count(&RN, FAS_EVENT_BIG_JANK, last, ~0ULL) == 0, "jank after resume");

	{
		u64 onset;

		rng_state = 5001;
		TR.n = 0;
		TR.nominal = 0;
		seg(&TR, 3, 1000.0 / 60, 1000.0 / 60, 0.3);
		seg(&TR, 5, 20, 20, 0.3);
		onset = TR.ns[TR.n - 1];
		seg(&TR, 10, 1000.0 / 60, 1000.0 / 60, 0.3);
		run_trace(&RN, &TR, freq, fps, 1, 16.667, false, 0);
		soft = first_ms(&RN, FAS_EVENT_RECOVERED, onset);
		if (verbose)
			printf("recovery: %.0f ms\n", soft);
		CHECK(soft > 400 && soft < 800, "recovery %.0f", soft);
	}
}

static void t_acquire(u64 freq)
{
	double gr[] = { 30, 60, 45, 20, 62 };
	u32 want[] = { 30, 60, 60, 30, 60 };
	u32 fps[] = { 30, 60 };
	int j;

	for (j = 0; j < 5; j++) {
		int i, got = -1;

		rng_state = 6000 + j;
		TR.n = 0;
		TR.nominal = 0;
		seg(&TR, 10, 1000.0 / gr[j], 1000.0 / gr[j], 0.3);
		run_trace(&RN, &TR, freq, fps, 2, 16.667, false, 0);
		for (i = 0; i < RN.n; i++)
			if (RN.e[i].type == FAS_EVENT_RATE_SWITCH) {
				got = (int)RN.e[i].fps;
				break;
			}
		if (verbose)
			printf("acquire %.0f fps -> %d\n", gr[j], got);
		CHECK(got == (int)want[j], "acquired %d want %u", got, want[j]);
	}
}

static void t_skew(u64 freq)
{
	u32 fps[] = { 60 };
	int i, swaps = 0, bad, small;

	rng_state = 7000;
	TR.n = 0;
	TR.nominal = 0;
	seg(&TR, 120, 1000.0 / 60, 1000.0 / 60, 0.3);
	for (i = 50; i + 1 < TR.n; i += 100, swaps++) {
		u64 tmp = TR.ns[i];

		TR.ns[i] = TR.ns[i + 1];
		TR.ns[i + 1] = tmp;
	}
	run_trace(&RN, &TR, freq, fps, 1, 16.667, false, 0);
	/*
	 * A swap drops one frame, so the detector sees one interval of two
	 * periods. That is one small hitch. The late timestamp must not
	 * look like a pause or a huge interval.
	 */
	small = count(&RN, FAS_EVENT_SMALL_JANK, 0, ~0ULL);
	bad = count(&RN, FAS_EVENT_BIG_JANK, 0, ~0ULL) +
	      count(&RN, FAS_EVENT_DEGRADED, 0, ~0ULL) +
	      count(&RN, FAS_EVENT_PAUSED, 0, ~0ULL) +
	      count(&RN, FAS_EVENT_RESUMED, 0, ~0ULL);
	if (verbose)
		printf("out of order timestamps: %d swaps, %d small hitches, %d other events\n",
		       swaps, small, bad);
	CHECK(bad == 0, "unexpected events %d", bad);
	CHECK(small == swaps, "small hitches %d for %d swaps", small, swaps);
}

static void t_setup(u64 freq)
{
	struct fas_cfg c;
	struct fas_hot h;
	u32 ok[] = { 30, 60, 120 };
	u32 dup[] = { 60, 60 };
	u32 close[] = { 60, 63 };
	u32 zero[] = { 0 };
	u32 big[] = { 1001 };
	u32 nine[] = { 1, 2, 3, 4, 5, 6, 7, 8, 9 };

	CHECK(!fas_det_setup(&c, &h, freq, ok, 3, 0, false), "valid list");
	CHECK(c.tgt[0].fps == 120 && c.tgt[2].fps == 30, "sort order");
	CHECK(fas_det_setup(&c, &h, freq, dup, 2, 0, false) == -EINVAL, "duplicate");
	CHECK(fas_det_setup(&c, &h, freq, close, 2, 0, false) == -EINVAL, "close rates");
	CHECK(fas_det_setup(&c, &h, freq, zero, 1, 0, false) == -EINVAL, "zero rate");
	CHECK(fas_det_setup(&c, &h, freq, big, 1, 0, false) == -EINVAL, "1001 fps");
	CHECK(fas_det_setup(&c, &h, freq, nine, 9, 0, false) == -EINVAL, "nine rates");
	CHECK(fas_det_setup(&c, &h, freq, ok, 0, 0, false) == -EINVAL, "empty list");
	CHECK(fas_det_setup(&c, &h, freq, ok, 3, freq / 100000, false) == -EINVAL, "tiny vsync");
}

int main(int argc, char **argv)
{
	u64 freqs[] = { 19200000ULL, 1000000000ULL };
	unsigned i;

	verbose = argc > 1 && argv[1][0] == '-' && argv[1][1] == 'v';

	for (i = 0; i < 2; i++) {
		u64 freq = freqs[i];

		if (verbose)
			printf("== counter %llu Hz\n", (unsigned long long)freq);
		t_setup(freq);
		t_reciprocal(freq);
		t_healthy(freq);
		t_hitches(freq);
		t_deficit(freq);
		t_rates(freq);
		t_pause_recover(freq);
		t_acquire(freq);
		t_skew(freq);
	}

	printf(failures ? "FAILED: %d checks\n" : "PASSED\n", failures);
	return failures ? 1 : 0;
}

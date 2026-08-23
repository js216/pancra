# SPDX-License-Identifier: GPL-3.0
# mpc.py --- the glucose forward model
# Copyright 2026 Jakob Kastelic
"""A one-step-ahead forward model of glucose, and a forecast built from it.

Given the CGM sample just taken and what has been eaten, injected and walked
since, it predicts the next sample five minutes out; forecast() rolls that
forward for as long as you ask. It decides nothing.

    G[k+1|k] = G[k]                    the CGM sample, which anchors it
             + b[tod]                  a time-of-day baseline drift
             + sum_j (f_j * q_j)[k]    each food's own learned curve
             - (a * w)[k]              exercise, lowering glucose directly
             - A[k] * (1 + (s * w)[k]) ...and making insulin work harder

    A[k] = kg_slow * (g0_slow * i_slow)[k] + kg_fast * (g0_fast * i_fast)[k]

Everything on the right is measured (G, q, w, i) or learned (b, f, a, s, kg).
The learned part is theta, fitted by recursive least squares with a DIAGONAL
covariance: one update per sample, no rescan of history, so the cost per
sample does not grow with how long it has been running.

EVERY FOOD HAS ITS OWN CURVE, and each insulin its own -- a cookie is in the
blood inside fifteen minutes where beans take an hour, and glargine is still
releasing on day two where aspart is spent in six. What differs between them
is timing as much as amount, and a model with one shape for "carbohydrate"
cannot say so.

WHAT A CURVE IS MADE OF. A food's curve is a non-negative mix of four fixed
speeds (peaks near 12, 30, 65 and 120 minutes); the two insulin curves are
fixed outright and only their gains are learned. The alternative -- an
independent tap every five minutes -- is 72 free numbers per food, and nothing
in it says a tap should resemble its neighbour, so with the meals a person
actually logs it fits noise. Four weights is what a handful of meals can
carry, and the food still ends up with a curve of its own.

MISSING INPUTS ARE THE NORMAL CASE. Food and exercise are logged when somebody
remembers. An unlogged meal is not a wrong input, it is no input: the food
term contributes nothing and the anchor and the baseline carry the sample. The
terms are learned separately, so a stretch with nothing logged still teaches
the baseline, the insulin gains and the exercise response.

USAGE

    from mpc import MPC, NFOOD

    m = MPC()
    slot = m.food_slot(food_type_id)          # one slot per food, or -1
    q = [0.0] * NFOOD
    q[slot] = 45.0                            # grams since the last call

    for t, glu in samples:                    # in order, one per reading
        m.update(glu, minute_of_day=t.hour * 60 + t.minute, q=q,
                 w=0.0, i_slow=0.0, i_fast=4.0)

    m.forecast(t.hour * 60 + t.minute, 12)    # the next hour, five-minutely
    m.curve(slot)                             # what that food has learned
    m.response(slot, 45)                      # what 45 g of it would do
"""

import numpy as np

# THE SAMPLE INTERVAL, in minutes. Every lag count is a duration divided by
# this, so a CGM with a different cadence changes one number.
DT_MIN = 5

# How many bins the day is split into for the baseline: 48 = half-hour bins,
# fine enough to separate dawn from mid-morning and coarse enough that each
# bin sees a sample every day rather than every few.
TOD_BINS = 48

# How far back each input reaches: the shortest window that still holds the
# effect. Food 6 h, exercise 12 h (it raises insulin sensitivity long after it
# ends), basal 48 h, bolus 6 h.
FOOD_LAGS = 72
WALK_LAGS = 144
SLOW_LAGS = 576
FAST_LAGS = 72

# How many foods are learned at once. A person eats a few things at a time;
# see food_slot for what happens when they are all taken.
NFOOD = 8

# The two insulin channels, by index.
INS_SLOW = 0
INS_FAST = 1

# No food in this slot. Food type ids start at 1, so 0 is free to mean empty.
FOOD_NONE = 0

# ---- THE SHAPES EACH CURVE IS BUILT FROM --------------------------------
#
# (p0, ksp, ka) for the two-compartment absorption below. The food set spans
# fast sugar to a slow mixed meal; a food learns a non-negative weight on each
# and its curve is the mix.
FOOD_RATES = [(0.85, 0.060, 0.130),    # peak ~12 min
              (0.90, 0.030, 0.070),    # ~30 min
              (0.92, 0.014, 0.034),    # ~65 min
              (0.94, 0.007, 0.016)]    # ~120 min
NFOODB = len(FOOD_RATES)

# Exercise acts while it happens and for hours afterwards. Three decay times
# rather than 144 free taps, for the same reason.
WALK_TAUS = [30.0, 120.0, 480.0]
NWALKB = len(WALK_TAUS)

# THE PARAMETER VECTOR'S LAYOUT. Spelled out as offsets because the update,
# the prediction and any reader of a curve all index it.
OFF_FOOD = 0
OFF_WALK_A = OFF_FOOD + (NFOOD * NFOODB)
OFF_WALK_S = OFF_WALK_A + NWALKB
OFF_KG = OFF_WALK_S + NWALKB
OFF_LEVEL = OFF_KG + 2
OFF_BASE = OFF_LEVEL + 1
NPARAM = OFF_BASE + TOD_BINS

# THE PRIORS. std0 is how wrong a parameter may be at the start, so it sets
# how fast it moves early on; qstd is how much it may drift per sample, which
# keeps the model tracking a body that changes rather than converging once and
# freezing; eta damps the step where a fast fit is worse than a slow one.
#
# A PRIOR IS A CLAIM ABOUT SIZE, and making it small is not the safe choice.
# The gain applied to one observation is eta*P*phi/(R + s): with R = 64 and a
# portion measured in hectograms, a food prior of 1.0 moves a weight by about
# 0.002 mg/dL per meal against the hundreds it needs, so the estimator sits at
# its prior for ever and the model predicts no response to food at all.
#
# WHERE 150 COMES FROM, and it is not the fit: a hundred grams of a starchy
# food is roughly fifty grams of carbohydrate, and without insulin ten grams
# of carbohydrate moves glucose thirty to fifty mg/dL. So a food's whole curve
# should sum to something in the low hundreds per hectogram, and the prior has
# to allow that before any meal is seen. Fitted with a prior of 25 the model
# predicted meal rises at half their real size -- the direction right and the
# amount wrong, which is the shape of a prior that is holding the answer down.
STD0_FOOD, QSTD_FOOD, ETA_FOOD = 150.0, 1e-3, 1.0
STD0_WALK_A, QSTD_WALK_A, ETA_WALK_A = 8.0, 1e-3, 1.0
STD0_WALK_S, QSTD_WALK_S, ETA_WALK_S = 0.15, 1e-5, 0.2
STD0_KG, QSTD_KG, ETA_KG = 60.0, 1e-4, 1.0
STD0_LEVEL, QSTD_LEVEL, ETA_LEVEL = 3.0, 1e-4, 0.5
STD0_BASE, QSTD_BASE, ETA_BASE = 0.5, 1e-4, 0.1

# WHERE GLUCOSE IS PULLED BACK TOWARDS, and the scale the level term is
# measured in. The reference only conditions the arithmetic -- the baseline
# supplies the constant, so the level this person actually returns to is
# whatever the pair of them implies, not this number.
LEVEL_REF = 140.0
LEVEL_SCALE = 0.01

# The measurement noise, as a variance: R is what stops the estimator chasing
# the sensor's own error.
SIGMA_CGM = 8.0
R = SIGMA_CGM * SIGMA_CGM

# THE FAST DRIFT STATE catches what the parameters cannot: an illness, a bad
# infusion site, a day that is simply off. RHO_D is its decay per sample (0.98
# is a memory of about four hours) and KD how much of each error it absorbs.
# Deliberately not a learned parameter -- it exists to keep short-lived
# surprises out of the curves, which describe this person in general.
RHO_D = 0.98
KD = 0.05

# Portions arrive in GRAMS and are divided by 100 on the way in, so a weight
# is mg/dL per hectogram and a curve sums to the rise from 100 g.
FOOD_SCALE = 0.01

# Enough contiguous scored predictions to publish one (an hour at a CGM's
# cadence), and close enough over them, in mg/dL of mean absolute residual.
READY_SCORED = 12
READY_MAE = 30.0


def exp_neg(x):
    """e^-x for x >= 0, by range reduction and a five-term series.

    Relative error ~1.3e-12 for small arguments, doubling with each squaring
    the reduction performs. A negative x returns 1, its limit at 0, rather
    than growing without bound.
    """
    if not x > 0.0:
        return 1.0
    shift = 0
    while x > 0.03125:  # 1/32
        x *= 0.5
        shift += 1
        if shift > 60:
            return 0.0
    t = 1.0 - (x / 5.0)
    t = 1.0 - ((x * t) / 4.0)
    t = 1.0 - ((x * t) / 3.0)
    t = 1.0 - ((x * t) / 2.0)
    t = 1.0 - (x * t)
    while shift > 0:
        t *= t
        shift -= 1
    return t


def sqrt_(x):
    """Square root by Newton's method on an argument scaled into [0.5, 2).

    A non-positive x returns 0: a negative variance is rounding, not a
    measurement, and a NaN would spread into everything downstream.
    """
    if not x > 0.0:
        return 0.0
    scale = 1.0
    while x > 2.0:
        x *= 0.25
        scale *= 2.0
    while x < 0.5:
        x *= 4.0
        scale *= 0.5
    r = x
    for _ in range(5):
        r = 0.5 * (r + (x / r))
    return r * scale


def two_state(n, p0, ksp, ka):
    """The two-compartment absorption every fixed shape here is built from.

    A fraction p0 starts in a depot that dissolves at ksp per minute; the rest
    starts soluble and is absorbed at ka per minute. What reaches the blood in
    each sample interval is the DIFFERENCE of the absorbed fraction across it,
    which makes this a causal impulse response rather than a cumulative curve.
    Normalised to unit area: the shape is a prior and the amplitude is learned.
    """
    out = np.zeros(n)
    s0 = 1.0 - p0
    prev = 0.0
    total = 0.0
    for j in range(n):
        t = float(j + 1) * float(DT_MIN)
        ep = exp_neg(ksp * t)
        ea = exp_neg(ka * t)
        pp = p0 * ep
        # The degenerate case is real: ka == ksp makes the denominator below
        # zero, and the limit as ka approaches ksp is a different formula.
        if ka - ksp < 1e-12 and ksp - ka < 1e-12:
            ss = (s0 + (ksp * p0 * t)) * ea
        else:
            ss = (s0 * ea) + (((ksp * p0) / (ka - ksp)) * (ep - ea))
        absorbed = 1.0 - pp - ss
        step = absorbed - prev
        # The continuous solution is monotone, so a negative difference is
        # rounding at the tail -- and a negative tap would mean an input
        # briefly acting the other way.
        out[j] = step if step > 0.0 else 0.0
        total += out[j]
        prev = absorbed
    if not total > 0.0:
        out[:] = 1.0 / float(n)
        return out
    return out / total


def g0_slow(n=SLOW_LAGS):
    """The basal profile: a long-acting analogue released from a depot for the
    better part of two days. Almost all of the dose starts undissolved, which
    delays the peak to about seven hours and flattens what follows."""
    return two_state(n, 0.95, 0.0012, 0.0035)


def g0_fast(n=FAST_LAGS):
    """The bolus profile: a rapid analogue peaking about forty-five minutes
    after the injection and nine tenths spent by four hours."""
    return two_state(n, 0.90, 0.011, 0.030)


def food_basis():
    """The four speeds a food's curve is mixed from."""
    return np.array([two_state(FOOD_LAGS, *r) for r in FOOD_RATES])


def walk_basis():
    """How long a session keeps acting: unit-area exponential decays."""
    out = []
    for tau in WALK_TAUS:
        k = np.array([exp_neg(((l + 0.5) * DT_MIN) / tau)
                      for l in range(WALK_LAGS)])
        out.append(k / k.sum())
    return np.array(out)


def tod_index(minute_of_day):
    """Which half-hour bin of the day a minute falls in."""
    mn = minute_of_day % 1440
    idx = (mn * TOD_BINS) // 1440
    return TOD_BINS - 1 if idx >= TOD_BINS else idx


def _priors():
    std0 = np.empty(NPARAM)
    qstd = np.empty(NPARAM)
    eta = np.empty(NPARAM)
    for lo, hi, s, q, e in (
            (OFF_FOOD, OFF_WALK_A, STD0_FOOD, QSTD_FOOD, ETA_FOOD),
            (OFF_WALK_A, OFF_WALK_S, STD0_WALK_A, QSTD_WALK_A, ETA_WALK_A),
            (OFF_WALK_S, OFF_KG, STD0_WALK_S, QSTD_WALK_S, ETA_WALK_S),
            (OFF_KG, OFF_LEVEL, STD0_KG, QSTD_KG, ETA_KG),
            (OFF_LEVEL, OFF_BASE, STD0_LEVEL, QSTD_LEVEL, ETA_LEVEL),
            (OFF_BASE, NPARAM, STD0_BASE, QSTD_BASE, ETA_BASE)):
        std0[lo:hi], qstd[lo:hi], eta[lo:hi] = s, q, e
    return std0, qstd, eta


class MPC:
    """One model. Feed it samples in order with update()."""

    def __init__(self):
        self.FB = food_basis()
        self.WB = walk_basis()
        self.g0_slow = g0_slow()
        self.g0_fast = g0_fast()

        self._std0, self._qstd, self._eta = _priors()
        self.theta = np.zeros(NPARAM)
        self.p = self._std0 * self._std0
        self.pending_phi = np.zeros(NPARAM)

        self.q_hist = np.zeros((NFOOD, FOOD_LAGS))   # newest at index 0
        self.w_hist = np.zeros(WALK_LAGS)
        self.i_slow = np.zeros(SLOW_LAGS)
        self.i_fast = np.zeros(FAST_LAGS)

        self.food_id = [FOOD_NONE] * NFOOD
        self.food_used = [0] * NFOOD

        self.d = 0.0             # the fast unmodelled drift state
        self.pending = 0.0       # last prediction, awaiting its sample
        self.have_pending = False
        self.last_error = 0.0    # newest residual, mg/dL
        self.scored = 0          # CONTIGUOUS predictions checked against fact
        self.mae = 0.0           # mean absolute residual over those
        self.glu = 0.0           # the CGM value the state is anchored to
        self.k = 0               # samples processed

        # The five terms of the last prediction, signed the way they act on
        # glucose. Diagnostic: nothing reads them back.
        self.t_base = self.t_food = self.t_walk = self.t_level = 0.0
        self.t_insulin = self.t_drift = self.t_sens = 0.0

    # ---- what each input has taught it ---------------------------------

    def curve(self, slot):
        """This food's absorption curve: mg/dL per 100 g per five minutes, by
        lag. Its own shape, mixed from the four speeds."""
        i = OFF_FOOD + slot * NFOODB
        return self.theta[i:i + NFOODB] @ self.FB

    def response(self, slot, grams, steps=48):
        """What this model says `grams` of one food would do, as the running
        total by lag -- the answer to "what does a cookie do to me"."""
        return np.cumsum(self.curve(slot)[:steps]) * grams * FOOD_SCALE

    def walk_curve(self):
        """How exercise acts on glucose directly, by lag: mg/dL per level per
        five minutes, positive meaning it lowers glucose."""
        return self.theta[OFF_WALK_A:OFF_WALK_S] @ self.WB

    def walk_response(self, level, minutes, steps=144):
        """What this model says `minutes` at `level` would do, as the running
        total by lag. Negative: exercise lowers glucose."""
        n = max(1, int(minutes // DT_MIN))
        w = np.zeros(WALK_LAGS)
        w[:n] = level
        out = []
        acc = 0.0
        for s in range(steps):
            acc -= float(np.dot(self.theta[OFF_WALK_A:OFF_WALK_S],
                                self.WB @ np.roll(w, s)))
            out.append(acc)
        return np.array(out)

    def gain(self, channel):
        """The learned gain for one insulin channel: mg/dL per unit."""
        if channel not in (INS_SLOW, INS_FAST):
            return 0.0
        return float(self.theta[OFF_KG + channel])

    def food_slot(self, food_id):
        """The slot this food type uses, assigning one if it has none.

        Returns 0..NFOOD-1, or -1 for FOOD_NONE. When every slot is taken the
        LEAST RECENTLY EATEN is evicted and its weights cleared -- a real loss
        of learning. What it must never do is hand two foods one slot: a curve
        fitted to bread and rice at once describes neither.
        """
        if food_id == FOOD_NONE:
            return -1
        for j in range(NFOOD):
            if self.food_id[j] == food_id:
                self.food_used[j] = self.k
                return j
        for j in range(NFOOD):
            if self.food_id[j] == FOOD_NONE:
                self.food_id[j] = food_id
                self.food_used[j] = self.k
                return j
        # The weights, their variances AND the portion history reset together:
        # leaving the history would have the new food's first prediction
        # respond to the previous food's portions.
        v = min(range(NFOOD), key=lambda j: self.food_used[j])
        i = OFF_FOOD + v * NFOODB
        self.theta[i:i + NFOODB] = 0.0
        self.p[i:i + NFOODB] = self._std0[i:i + NFOODB] ** 2
        self.q_hist[v, :] = 0.0
        self.food_id[v] = food_id
        self.food_used[v] = self.k
        return v

    # ---- the estimator --------------------------------------------------

    def _regressors(self):
        """Each input seen through its own shapes: the regressor its weights
        multiply."""
        fconv = (self.q_hist @ self.FB.T).reshape(-1)
        wconv = self.WB @ self.w_hist
        prof_slow = float(np.dot(self.g0_slow, self.i_slow))
        prof_fast = float(np.dot(self.g0_fast, self.i_fast))
        return fconv, wconv, prof_slow, prof_fast

    def update(self, glu, minute_of_day, q=None, w=0.0, i_slow=0.0,
               i_fast=0.0, learn=True):
        """One sample. Returns the prediction for the NEXT one.

        glu             the new CGM value, mg/dL
        minute_of_day   local time 0..1439 -- the baseline term is a clock
        q               grams of each food eaten since the last call, BY SLOT,
                        or None when nothing was logged
        w               exercise intensity over the interval (level 0..3)
        i_slow, i_fast  units injected since the last call
        learn           False replays a sample without fitting to it, for a
                        gap where the inputs are not trustworthy
        """
        if self.have_pending:
            # THE PREDICTION BEING SCORED WAS MADE ONE CALL AGO, against the
            # regressor saved with it. Scoring it BEFORE the new inputs go in
            # is what makes this one-step-ahead rather than a fit to data it
            # has already seen.
            error = glu - self.pending
            self.last_error = error
            aerr = abs(error)
            if learn:
                # `learn` is what says the two samples are contiguous: across
                # a gap the indices no longer line up, so the run of evidence
                # breaks rather than being claimed across it.
                self.scored += 1
                self.mae = aerr if self.scored == 1 else \
                    self.mae + ((aerr - self.mae) / 8.0)
            else:
                self.scored = 0
                self.mae = aerr
            self.d = (RHO_D * self.d) + (KD * error)
            # Not learning still ages the covariance: saying the parameters
            # are as well known after an unobserved hour as before it would
            # have the next real sample trusted too little.
            self.p += self._qstd * self._qstd
            if learn:
                phi = self.pending_phi
                v = self.p * phi
                s = float(np.dot(phi, v))
                if R + s > 0.0:
                    keff = self._eta * (v / (R + s))
                    self.theta += keff * error
                    # JOSEPH FORM, diagonal. The shorter P = (1 - K phi) P is
                    # only equivalent for the exact optimal gain, and eta
                    # scales the gain away from it deliberately; with a damped
                    # gain the short form can drive a variance negative.
                    self.p = self.p - (2.0 * keff * v) + \
                        ((s + R) * keff * keff)
                    np.maximum(self.p, 0.0, out=self.p)
                    # NOTHING HERE MAY CHANGE SIGN. Food raises glucose,
                    # insulin lowers it, exercise lowers it -- none of that is
                    # in question, and a negative weight is what a handful of
                    # noisy samples looks like before the fit settles.
                    for lo, hi in ((OFF_FOOD, OFF_WALK_A),
                                   (OFF_WALK_A, OFF_KG),
                                   (OFF_KG, OFF_LEVEL)):
                        np.maximum(self.theta[lo:hi], 0.0,
                                   out=self.theta[lo:hi])

        # ANCHOR TO THE MEASUREMENT. The model predicts a CHANGE: every
        # prediction starts from the CGM value rather than from the previous
        # prediction, so an error cannot accumulate into a drift.
        self.glu = float(glu)

        for j in range(NFOOD):
            self.q_hist[j, 1:] = self.q_hist[j, :-1]
            self.q_hist[j, 0] = (q[j] * FOOD_SCALE) if q is not None else 0.0
        self.w_hist[1:] = self.w_hist[:-1]; self.w_hist[0] = w
        self.i_slow[1:] = self.i_slow[:-1]; self.i_slow[0] = i_slow
        self.i_fast[1:] = self.i_fast[:-1]; self.i_fast[0] = i_fast

        fconv, wconv, prof_slow, prof_fast = self._regressors()
        food = float(np.dot(self.theta[OFF_FOOD:OFF_WALK_A], fconv))
        walk_acute = float(np.dot(self.theta[OFF_WALK_A:OFF_WALK_S], wconv))
        walk_sens = float(np.dot(self.theta[OFF_WALK_S:OFF_KG], wconv))
        action = (self.theta[OFF_KG + INS_SLOW] * prof_slow +
                  self.theta[OFF_KG + INS_FAST] * prof_fast)
        tod = tod_index(minute_of_day)
        baseline = float(self.theta[OFF_BASE + tod])
        # GLUCOSE COMES BACK. A level well above where this person sits falls
        # of its own accord and one below it rises, whatever else is going on
        # -- it is the largest single thing a forecast can know, and a model
        # that only ever ADDS effects to the current reading cannot say it.
        lvl = (self.glu - LEVEL_REF) * LEVEL_SCALE
        reversion = float(self.theta[OFF_LEVEL]) * lvl

        phi = self.pending_phi
        phi[:] = 0.0
        phi[OFF_FOOD:OFF_WALK_A] = fconv
        phi[OFF_WALK_A:OFF_WALK_S] = -wconv
        # The sensitivity term multiplies the insulin ACTION, so its
        # derivative carries that action -- which is why a walk with no
        # insulin on board teaches this block nothing, correctly.
        phi[OFF_WALK_S:OFF_KG] = -action * wconv
        phi[OFF_KG + INS_SLOW] = -prof_slow * (1.0 + walk_sens)
        phi[OFF_KG + INS_FAST] = -prof_fast * (1.0 + walk_sens)
        phi[OFF_LEVEL] = lvl
        phi[OFF_BASE + tod] = 1.0

        pred = self.glu + baseline + reversion + food - walk_acute - \
            (action * (1.0 + walk_sens)) + self.d

        # The terms as they were used, signed the way they act on glucose --
        # which is not the way two of them appear in the line above.
        self.t_base = baseline
        self.t_level = reversion
        self.t_food = food
        self.t_walk = -walk_acute
        self.t_insulin = -(action * (1.0 + walk_sens))
        self.t_drift = self.d
        self.t_sens = walk_sens

        self.pending = float(pred)
        self.have_pending = True
        self.k += 1
        return self.pending

    # ---- looking further ahead ------------------------------------------

    def forecast(self, minute_of_day, steps):
        """`steps` five-minute predictions from the state as it stands.

        The histories roll forward and nothing new arrives -- no food, no
        insulin, no exercise -- which is the honest reading of "if nothing
        else happens". What is already in the pipeline goes on acting.

        THE DRIFT STATE IS LEFT OUT. `d` absorbs a share of each residual with
        a four-hour memory, which is what keeps a bad day out of the curves;
        carried across twelve steps it becomes d x 10.6 of extrapolated noise,
        and over four months of this record it costs 2.3 mg/dL of hour-ahead
        accuracy. It belongs in the one-step prediction and not here.

        The model's own state is untouched: this is a question, not a step.
        """
        q = self.q_hist.copy()
        w = self.w_hist.copy()
        islow = self.i_slow.copy()
        ifast = self.i_fast.copy()
        glu = self.glu
        out = []
        for s in range(steps):
            q[:, 1:] = q[:, :-1]; q[:, 0] = 0.0
            w[1:] = w[:-1]; w[0] = 0.0
            islow[1:] = islow[:-1]; islow[0] = 0.0
            ifast[1:] = ifast[:-1]; ifast[0] = 0.0
            fconv = (q @ self.FB.T).reshape(-1)
            wconv = self.WB @ w
            action = (self.theta[OFF_KG + INS_SLOW] *
                      float(np.dot(self.g0_slow, islow)) +
                      self.theta[OFF_KG + INS_FAST] *
                      float(np.dot(self.g0_fast, ifast)))
            ws = float(np.dot(self.theta[OFF_WALK_S:OFF_KG], wconv))
            glu = (glu
                   + self.theta[OFF_LEVEL] * ((glu - LEVEL_REF) * LEVEL_SCALE)
                   + self.theta[OFF_BASE + tod_index(
                       minute_of_day + (s + 1) * DT_MIN)]
                   + float(np.dot(self.theta[OFF_FOOD:OFF_WALK_A], fconv))
                   - float(np.dot(self.theta[OFF_WALK_A:OFF_WALK_S], wconv))
                   - action * (1.0 + ws))
            out.append(float(glu))
        return out

    # ---- how sure it is --------------------------------------------------

    def sigma(self):
        """The 1-sigma PARAMETER uncertainty on the pending prediction, mg/dL.

        phi' P phi, a weighted sum of squares with P diagonal. How sure the
        model is of its own coefficients -- not how noisy the sensor is, nor
        what it does not describe at all. It is the part that shrinks visibly
        as the model learns. Zero before the first prediction.
        """
        if not self.have_pending:
            return 0.0
        return sqrt_(float(np.dot(self.pending_phi * self.pending_phi, self.p)))

    def sigma_total(self):
        """The WHOLE predictive uncertainty, mg/dL: the coefficients, plus the
        sensor's own error, plus the drift state -- the model's estimate of
        what it is not describing. Variances add."""
        par = self.sigma()
        return sqrt_((par * par) + R + (self.d * self.d))

    def ready(self):
        """Is this model worth publishing? Enough CONTIGUOUS scored
        predictions AND residuals within READY_MAE over them. Both halves are
        needed: a model that has scored fifty predictions and is out by 60
        mg/dL each time has been tested and has failed."""
        return self.scored >= READY_SCORED and self.mae <= READY_MAE

"""a scenario that edits a lot through the front panel, the way a person would, so that the
firmware's own autosave (tmp.snd / tmp.pat in /tmp) captures it: every voice, every
parameter page (osc aeg mod fm click filter lfo mix), all 4 knobs turned, then a step
pattern programmed on every track, played, stopped, and left 3 s for the autosave.
usage: tools/groove-edit.py <name> <card> <seed>   (writes scenario/<name>.txt)
button numbers: buttonHandler.h ButtonNumbers. knobs: adc 2k / 2k+1 (two wipers, see
4.web/site/main.js wipers())."""
import sys, random

name, card, seed = sys.argv[1], sys.argv[2], int(sys.argv[3])
rnd = random.Random(seed)
SEL = [16, 17, 18, 19, 8, 9, 10, 11]                  # osc aeg mod fm click filter lfo mix
STEP = [39, 38, 37, 36, 31, 30, 29, 28, 23, 22, 21, 20, 15, 14, 13, 12]
ev, t = [], 3000
angle = [90.0] * 4


def wipers(deg):
    x = deg % 360
    b = x if x <= 180 else 360 - x
    a = 90 + x if x <= 90 else (270 - x if x <= 270 else x - 270)
    raw = lambda d: int(round(d / 180 * 0xfff)) << 4
    return raw(a), raw(b)


def press(n, hold=60, gap=120):
    global t
    ev.append((t, f"button {n} 1")); ev.append((t + hold, f"button {n} 0")); t += hold + gap


for voice in range(1, 8):
    press(voice)
    for sel in SEL:
        press(sel)
        for k in range(4):
            turn = rnd.choice([-1, 1]) * rnd.randint(4, 14)    # detents of 2 degrees
            for _ in range(abs(turn)):
                angle[k] += 2 if turn > 0 else -2
                a, b = wipers(angle[k])
                ev.append((t, f"adc {2 * k} {a}")); ev.append((t, f"adc {2 * k + 1} {b}")); t += 30
            t += 60
# a pattern: every track gets its own steps
for voice in range(1, 8):
    press(voice)
    steps = sorted(rnd.sample(range(16), rnd.randint(3, 9)))
    for s in steps: press(STEP[s], gap=60)
press(33)                    # play
t += 4000
press(33)                    # stop: the pattern autosave timer starts
t += 3500                    # > PRESET_SAVE_TIME (2000 ms) after the last edit
ev.sort(key=lambda e: e[0])
with open(f"scenario/{name}.txt", "w") as f:
    f.write(f"# {name}: panel edits on every voice and page, a pattern, play, stop, autosave (tools/groove-edit.py)\n")
    f.write(f"card {card}\nseed c0ffee\n")
    f.write("\n".join(f"{tt} {l}" for tt, l in ev) + f"\n{t} end\n")
print(name, len(ev), "events,", t, "ms")

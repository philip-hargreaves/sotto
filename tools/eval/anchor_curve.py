# The voiceprint learning curve: how strongly the doctor cluster matched the stored print
# at each of one clinician's consultations, from a cold start and from an enrolment.
#   python tools/eval/anchor_curve.py <cold tag> <enrolled tag> <day prefix> <out.png>
# Writes the figure and a CSV beside it.
import csv
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from anchor_sims import seals  # noqa: E402

cold_tag, enrolled_tag, day, out = sys.argv[1:5]


def curve(tag):
    rows = [r for r in seals(tag) if r[0].startswith(day)]
    rows.sort(key=lambda r: r[0])
    points = []
    for i, (consult, sims, margin, doctor) in enumerate(rows, 1):
        doc = sims[doctor] if sims and 0 <= doctor < len(sims) else float("nan")
        points.append((i, consult, doc, margin, doctor))
    return points


cold, enrolled = curve(cold_tag), curve(enrolled_tag)
with open(os.path.splitext(out)[0] + ".csv", "w", newline="", encoding="utf-8") as f:
    w = csv.writer(f)
    w.writerow(["consultation", "consult", "cold_doctor_similarity", "cold_margin",
                "enrolled_doctor_similarity", "enrolled_margin"])
    for c, e in zip(cold, enrolled):
        w.writerow([c[0], c[1], f"{c[2]:.3f}", f"{c[3]:.3f}", f"{e[2]:.3f}", f"{e[3]:.3f}"])

import matplotlib  # noqa: E402

matplotlib.use("Agg")
import matplotlib.pyplot as plt  # noqa: E402

fig, ax = plt.subplots(figsize=(7, 4))
ax.plot([p[0] for p in cold], [p[2] for p in cold], marker="o", label="learned from consultations (cold start)")
ax.plot([p[0] for p in enrolled], [p[2] for p in enrolled], marker="s", label="enrolled from a 45 s reading")
# The safeguard's floor and where other clinicians' prints landed on the same clinician
ax.axhspan(0.44, 0.745, color="grey", alpha=0.15, lw=0)
ax.text(15.3, 0.59, "another\nclinician's\nprint", fontsize=8, color="dimgrey", va="center", ha="left")
ax.axhline(0.80, color="black", ls="--", lw=1)
ax.text(15.3, 0.825, "floor 0.80", fontsize=8, va="center", ha="left")
if cold and cold[0][2] != cold[0][2]:  # no print yet: the content named the roles
    ax.annotate("no print yet:\nroles from content", xy=(1, 0.02), xytext=(1.3, 0.2), fontsize=8,
                arrowprops=dict(arrowstyle="->", color="grey"), color="dimgrey")
ax.set_xlim(0.5, 17.5)
ax.set_xlabel("consultation (one clinician, in order)")
ax.set_ylabel("doctor's similarity to the stored voiceprint (cosine)")
ax.set_ylim(0, 1)
ax.set_xticks([p[0] for p in cold])
ax.grid(alpha=0.3)
ax.legend(loc="lower right")
fig.tight_layout()
fig.savefig(out, dpi=160)
print("wrote", out, "and its csv")
for c, e in zip(cold, enrolled):
    print(f"  {c[0]:2d} {c[1]:22s} cold {c[2]:.3f} (margin {c[3]:.3f}, doctor {c[4]})   "
          f"enrolled {e[2]:.3f} (margin {e[3]:.3f}, doctor {e[4]})")

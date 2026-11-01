#!/usr/bin/env python3
"""
analyze.py - analyses voip-results.csv and draws the figures (English labels).

Kör i samma mapp som voip-results.csv:
    python3 analyze.py
eller med annan fil:
    python3 analyze.py min-fil.csv

Skapar:
    summary.csv           medelvärden och 95 % konfidensintervall per N, riktning och QoS
    fig_delay.png         fördröjning mot antal nedladdare
    fig_jitter.png        jitter mot antal nedladdare
    fig_loss.png          paketförlust mot antal nedladdare
    fig_mos.png           R-värde och MOS (E-modellen) mot antal nedladdare
    fig_tcp.png           total TCP-genomströmning mot antal nedladdare

Kräver: pip install pandas matplotlib
"""

import math
import sys

import matplotlib

matplotlib.use("Agg")  # rita till fil, inget fönster behövs (fungerar i WSL)
import matplotlib.pyplot as plt
import pandas as pd

# ------------------------------------------------------------------
# Antaganden för mun-till-öra-fördröjning (skriv dessa i artikeln!)
# ------------------------------------------------------------------
PACKETIZATION_MS = 20.0  # en G.711-ram på 20 ms måste fyllas innan den skickas
JITTER_BUFFER_MS = 40.0  # antagen fast jitterbuffert i mottagaren

# Kvalitetsgränser
DELAY_LIMIT_MS = 150.0  # ITU-T G.114, mun-till-öra
JITTER_GOOD_MS = 30.0
JITTER_BAD_MS = 50.0
LOSS_GOOD_PCT = 1.0
LOSS_BAD_PCT = 3.0
R_ACCEPTABLE = 70.0

# t-värden för 95 % konfidensintervall (tvåsidigt), index = frihetsgrader
T95 = {1: 12.706, 2: 4.303, 3: 3.182, 4: 2.776, 5: 2.571, 6: 2.447, 7: 2.365,
       8: 2.306, 9: 2.262, 10: 2.228, 14: 2.145, 19: 2.093, 29: 2.045}


def t_value(dof):
    if dof <= 0:
        return float("nan")
    if dof in T95:
        return T95[dof]
    smaller = [d for d in T95 if d <= dof]
    return T95[max(smaller)] if smaller else 1.96


def e_model(mouth_to_ear_ms, loss_fraction):
    """Förenklad E-modell för G.711 (Cole & Rosenbluth, 2001)."""
    d = mouth_to_ear_ms
    i_d = 0.024 * d + (0.11 * (d - 177.3) if d > 177.3 else 0.0)
    i_e = 30.0 * math.log(1.0 + 15.0 * loss_fraction)
    r = 94.2 - i_d - i_e
    r = max(0.0, min(100.0, r))
    mos = 1.0 + 0.035 * r + 7e-6 * r * (r - 60.0) * (100.0 - r)
    mos = max(1.0, min(4.5, mos))
    return r, mos


def main():
    path = sys.argv[1] if len(sys.argv) > 1 else "voip-results.csv"
    df = pd.read_csv(path)

    # Mun-till-öra-fördröjning och E-modellen per körning
    df["mouth_to_ear_ms"] = df["delay_ms"] + PACKETIZATION_MS + JITTER_BUFFER_MS
    rm = df.apply(lambda row: e_model(row["mouth_to_ear_ms"], row["loss_pct"] / 100.0), axis=1)
    df["r_value"] = [x[0] for x in rm]
    df["mos"] = [x[1] for x in rm]

    metrics = ["delay_ms", "delay_p95_ms", "mouth_to_ear_ms", "jitter_ms",
               "loss_pct", "r_value", "mos", "tcp_mbps"]

    # Medelvärde och 95 % konfidensintervall över repetitionerna
    rows = []
    for (qos, direction, n), g in df.groupby(["qos", "direction", "n_downloaders"]):
        row = {"qos": qos, "direction": direction, "n_downloaders": n, "runs": len(g)}
        for m in metrics:
            mean = g[m].mean()
            if len(g) > 1:
                half = t_value(len(g) - 1) * g[m].std(ddof=1) / math.sqrt(len(g))
            else:
                half = 0.0
            row[m] = mean
            row[m + "_ci"] = half
        rows.append(row)
    summary = pd.DataFrame(rows).sort_values(["qos", "direction", "n_downloaders"])
    summary.to_csv("summary.csv", index=False, float_format="%.3f")
    print("Saved summary.csv")

    # Brytpunkt: första N där samtalet inte längre är acceptabelt
    print("\nBreaking points (first N where a limit is exceeded):")
    for (qos, direction), g in summary.groupby(["qos", "direction"]):
        label = f"{'voice queue (QoS)' if qos else 'best effort'}, {'downlink' if direction == 'down' else 'uplink'}"
        bad = g[(g["mouth_to_ear_ms"] > DELAY_LIMIT_MS) | (g["jitter_ms"] > JITTER_GOOD_MS)
                | (g["loss_pct"] > LOSS_GOOD_PCT) | (g["r_value"] < R_ACCEPTABLE)]
        first = int(bad["n_downloaders"].min()) if len(bad) else None
        print(f"  {label}: {'no limit exceeded' if first is None else f'N = {first}'}")

    # ------------------------------------------------------------------
    # Grafer
    # ------------------------------------------------------------------
    styles = {
        (0, "down"): dict(color="#c0392b", marker="o", label="Downlink, best effort"),
        (0, "up"): dict(color="#e67e22", marker="s", label="Uplink, best effort"),
        (1, "down"): dict(color="#2471a3", marker="o", linestyle="--", label="Downlink, QoS (AC_VO)"),
        (1, "up"): dict(color="#17a589", marker="s", linestyle="--", label="Uplink, QoS (AC_VO)"),
    }

    # Figures for a two-column IEEE paper: no title (the caption in the paper
    # describes the figure), legend outside the data, and confidence intervals
    # that never go below zero (a negative delay, jitter or loss is impossible).
    plt.rcParams.update({"font.size": 10})

    def plot(metric, ylabel, filename, limits=(), groups=None):
        fig, ax = plt.subplots(figsize=(6, 3.8))
        for (qos, direction), g in summary.groupby(["qos", "direction"]):
            if groups is not None and (qos, direction) not in groups:
                continue
            st = dict(styles.get((qos, direction), {}))
            if groups is not None:
                st["label"] = groups[(qos, direction)]
            y = g[metric].to_numpy()
            ci = g[metric + "_ci"].to_numpy()
            lower = [min(c, v) for v, c in zip(y, ci)]  # clip at zero
            ax.errorbar(g["n_downloaders"], y, yerr=[lower, ci],
                        capsize=4, linewidth=1.8, **st)
        for value, text, color in limits:
            ax.axhline(value, color=color, linestyle=":", linewidth=1.3)
            ax.text(ax.get_xlim()[1], value, f" {text}", va="center", fontsize=9, color=color)
        ax.set_xlabel("Number of concurrent downloaders (N)")
        ax.set_ylabel(ylabel)
        ax.set_xticks(sorted(df["n_downloaders"].unique()))
        ax.grid(True, alpha=0.3)
        ax.legend(fontsize=9, loc="upper center", bbox_to_anchor=(0.5, -0.2), ncol=2, frameon=False)
        fig.tight_layout()
        fig.savefig(filename, dpi=300, bbox_inches="tight")
        plt.close(fig)
        print(f"Saved {filename}")

    plot("mouth_to_ear_ms", "Mouth-to-ear delay (ms)", "fig_delay.png",
         [(DELAY_LIMIT_MS, "150 ms", "black")])
    plot("jitter_ms", "Mean jitter (ms)", "fig_jitter.png",
         [(JITTER_GOOD_MS, "30 ms", "darkorange"), (JITTER_BAD_MS, "50 ms", "red")])
    plot("loss_pct", "Packet loss (%)", "fig_loss.png",
         [(LOSS_GOOD_PCT, "1 %", "darkorange"), (LOSS_BAD_PCT, "3 %", "red")])
    plot("r_value", "R-factor (E-model)", "fig_mos.png",
         [(R_ACCEPTABLE, "R = 70", "black")])
    # TCP throughput is the same value for both directions of the call,
    # so only one line per priority setting is drawn.
    plot("tcp_mbps", "Total TCP throughput (Mbit/s)", "fig_tcp.png", (),
         groups={(0, "down"): "Best effort", (1, "down"): "QoS (AC_VO)"})


if __name__ == "__main__":
    main()

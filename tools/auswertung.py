#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
Laermtracker - statistische Auswertung der aufgezeichneten Ereignisse (EVENTS.CSV).

Aufruf (Windows):
    python auswertung.py D:\\laerm\\daten            Ordner mit EVENTS.CSV (oder direkt die CSV-Datei)
    python auswertung.py                            ohne Angabe oeffnet sich ein Ordner-Auswahldialog
Optionen:
    --ausgabe ORDNER         Zielordner fuer Diagramme/Berichte (Standard: <Datenordner>\\auswertung)
    --von 2026-10-01 --bis 2026-10-14                Zeitraum einschraenken
    --nacht-start 22 --nacht-ende 6                  Nachtzeit (Stunden)
    --kein-fenster           Ordner nicht automatisch oeffnen

Benoetigt:  pip install pandas matplotlib
Erzeugt:    PNG-Diagramme, auswertung.pdf (alle Seiten), kennzahlen.txt, tages_statistik.csv
"""
import argparse
import os
import sys

try:
    import numpy as np
    import pandas as pd
    import matplotlib
    matplotlib.use("Agg")
    import matplotlib.ticker
    import matplotlib.pyplot as plt
    from matplotlib.backends.backend_pdf import PdfPages
except ImportError as e:                                            # pragma: no cover
    print("Es fehlen Python-Pakete (%s).\nInstallation:  pip install pandas matplotlib" % e)
    sys.exit(1)

WOCHENTAGE = ["Mo", "Di", "Mi", "Do", "Fr", "Sa", "So"]
C_OK, C_ST, C_NACHT, C_NACHT_ST = "#6aa9d8", "#d32f2f", "#2b4c7e", "#8e1b1b"
HINWEIS = ("Pegel unkalibriert/indikativ: Messwerte eines MEMS- bzw. USB-Mikrofons, kein geeichtes Schallpegelmessgeraet; "
           "LAFmax = hoechster A-bewerteter Pegel (Fast, 125 ms).")


# --------------------------------------------------------------------------------------------- Einlesen
def num(s):
    """Zahl mit Komma oder Punkt als Dezimalzeichen; Unlesbares (z. B. von Excel in ein Datum verwandelt) wird NaN."""
    return pd.to_numeric(s.astype(str).str.replace(",", ".", regex=False).str.strip(), errors="coerce")


def finde_csv(pfad):
    if os.path.isfile(pfad):
        return pfad
    p = os.path.join(pfad, "EVENTS.CSV")
    if os.path.isfile(p):
        return p
    raise SystemExit("Keine EVENTS.CSV gefunden in: %s" % pfad)


def lade(pfad, nacht_start, nacht_ende):
    datei = finde_csv(pfad)
    df = pd.read_csv(datei, sep=";", dtype=str, encoding="utf-8-sig", keep_default_na=False)
    df.columns = [c.strip().lower() for c in df.columns]
    n_roh = len(df)
    for c in ("laeq_dba", "lafmax_dba", "lpeak_db", "lae_dba", "hintergrund_dba", "grenzwert_dba", "ueber_db", "aktiv_s"):
        df[c] = num(df[c]) if c in df else np.nan
    datum = pd.to_datetime(df["datum"], format="%Y-%m-%d", errors="coerce")
    alt = pd.to_datetime(df["datum"], dayfirst=True, errors="coerce")           # falls Excel das Datum umformatiert hat
    datum = datum.fillna(alt)
    zeit = pd.to_timedelta(df["zeit"].astype(str).str.strip(), errors="coerce")
    df["dt"] = datum + zeit
    kaputt = int(df[["dt", "lafmax_dba"]].isna().any(axis=1).sum())
    df = df.dropna(subset=["dt", "lafmax_dba"]).copy()
    df["stoerung"] = df.get("stoerung", "0").astype(str).str.strip().eq("1")
    df["stunde"] = df["dt"].dt.hour
    df["minute"] = df["dt"].dt.minute
    df["tag"] = df["dt"].dt.normalize()
    df["wochentag"] = df["dt"].dt.dayofweek
    if "zeitraum" in df:
        df["nacht"] = df["zeitraum"].astype(str).str.upper().str.strip().eq("NACHT")
    else:
        df["nacht"] = (df["stunde"] >= nacht_start) | (df["stunde"] < nacht_ende)
    # Nacht gehoert zum Abend, an dem sie beginnt
    df["nachttag"] = df["tag"]
    vor_mitternacht = df["nacht"] & (df["stunde"] < nacht_ende)
    df.loc[vor_mitternacht, "nachttag"] = df.loc[vor_mitternacht, "tag"] - pd.Timedelta(days=1)
    df = df.sort_values("dt").reset_index(drop=True)
    return df, datei, n_roh, kaputt


# --------------------------------------------------------------------------------------------- Kennzahlen
def pegelsumme(lae, sekunden):
    """Beitrag der Ereignisse zum Mittelungspegel: 10 lg( Summe 10^(LAE/10) / T )."""
    lae = lae.dropna()
    if lae.empty or sekunden <= 0:
        return float("nan")
    return 10.0 * np.log10(np.sum(10.0 ** (lae / 10.0)) / sekunden)


def kennzahlen(df, nacht_start, nacht_ende):
    z = []
    tage = pd.date_range(df["tag"].min(), df["tag"].max(), freq="D")
    n_tage = len(tage)
    t_nacht = ((24 - nacht_start) + nacht_ende) * 3600.0
    t_tag = 86400.0 - t_nacht
    pro_tag = df.groupby("tag").size().reindex(tage, fill_value=0)
    st = df["stoerung"]
    z.append("LAERMTRACKER - AUSWERTUNG")
    z.append("=" * 60)
    z.append("Zeitraum:            %s bis %s (%d Tage, %d Tage mit Ereignissen)" % (
        df["dt"].min().strftime("%d.%m.%Y %H:%M"), df["dt"].max().strftime("%d.%m.%Y %H:%M"), n_tage, (pro_tag > 0).sum()))
    z.append("Ereignisse gesamt:   %d  (%.1f je Tag, Median %.0f, Maximum %d an %s)" % (
        len(df), len(df) / n_tage, pro_tag.median(), pro_tag.max(), pro_tag.idxmax().strftime("%d.%m.%Y")))
    z.append("davon Stoerungen:    %d (%.0f %%)  [LAFmax >= Grenzwert]" % (st.sum(), 100.0 * st.mean()))
    z.append("davon nachts:        %d (%.0f %%), Stoerungen nachts: %d" % (df["nacht"].sum(), 100.0 * df["nacht"].mean(), (st & df["nacht"]).sum()))
    z.append("davon tagsueber:     %d (%.0f %%), Stoerungen tags:   %d" % ((~df["nacht"]).sum(), 100.0 * (~df["nacht"]).mean(), (st & ~df["nacht"]).sum()))
    z.append("")
    lf = df["lafmax_dba"]
    z.append("LAFmax je Ereignis [dB(A)]:  Median %.1f | 90%%: %.1f | 95%%: %.1f | Maximum %.1f" % (
        lf.median(), lf.quantile(0.9), lf.quantile(0.95), lf.max()))
    for name, m in (("nachts", df["nacht"]), ("tagsueber", ~df["nacht"])):
        if m.any():
            z.append("  %-10s Median %.1f | Maximum %.1f | Anzahl %d" % (name, lf[m].median(), lf[m].max(), m.sum()))
    z.append("")
    z.append("Beitrag der Ereignisse zum Mittelungspegel (aus LAE, ueber den ganzen Zeitraum):")
    z.append("  Tag   (%02d-%02d Uhr): %.1f dB(A)" % (nacht_ende, nacht_start, pegelsumme(df.loc[~df["nacht"], "lae_dba"], t_tag * n_tage)))
    z.append("  Nacht (%02d-%02d Uhr): %.1f dB(A)" % (nacht_start, nacht_ende, pegelsumme(df.loc[df["nacht"], "lae_dba"], t_nacht * n_tage)))
    z.append("  (nur die aufgezeichneten Ereignisse, ohne Hintergrundgeraeusch)")
    z.append("")
    h = df.groupby("stunde").size().reindex(range(24), fill_value=0)
    top = h.sort_values(ascending=False).head(3)
    z.append("Haeufigste Stunden:  " + ", ".join("%02d-%02d Uhr (%d)" % (i, i + 1, v) for i, v in top.items()))
    m = df.groupby("minute").size().reindex(range(60), fill_value=0)
    topm = m.sort_values(ascending=False).head(4)
    z.append("Haeufigste Minuten (Minute der Stunde): " + ", ".join(":%02d (%d)" % (i, v) for i, v in topm.items()))
    nah = ((df["minute"] <= 2) | (df["minute"] >= 58) | ((df["minute"] >= 28) & (df["minute"] <= 32))).mean()
    z.append("Anteil Ereignisse um :00 oder :30 (+-2 min): %.0f %%  (bei gleichmaessiger Verteilung waeren es etwa 17 %%)" % (100.0 * nah))
    if len(df) > 3:
        ab = df["dt"].diff().dt.total_seconds().dropna() / 60.0
        z.append("Abstand zwischen Ereignissen: Median %.1f min, 25%%: %.1f min, 75%%: %.1f min" % (ab.median(), ab.quantile(.25), ab.quantile(.75)))
    if n_tage >= 5:
        k = np.polyfit(np.arange(n_tage), pro_tag.values, 1)[0]
        z.append("Trend Ereignisse je Tag: %+.2f pro Tag (%s)" % (k, "zunehmend" if k > 0.2 else ("abnehmend" if k < -0.2 else "etwa gleichbleibend")))
    wd = df.groupby("wochentag").size().reindex(range(7), fill_value=0)
    nwd = pd.Series([int(((tage.dayofweek) == i).sum()) for i in range(7)], index=range(7)).replace(0, np.nan)
    z.append("Ereignisse je Wochentag (Mittel): " + ", ".join("%s %.1f" % (WOCHENTAGE[i], (wd[i] / nwd[i]) if nwd[i] == nwd[i] else 0) for i in range(7)))
    nn = df[df["nacht"]].groupby("nachttag").agg(n=("dt", "size"), mx=("lafmax_dba", "max"), st=("stoerung", "sum"))
    if len(nn):
        w = nn.sort_values(["st", "mx"], ascending=False).head(3)
        z.append("")
        z.append("Auffaelligste Naechte (Beginn am Abend): " + "; ".join(
            "%s: %d Ereignisse, %d Stoerungen, max %.1f dB(A)" % (i.strftime("%d.%m."), r.n, r.st, r.mx) for i, r in w.iterrows()))
    z.append("")
    z.append("Hinweis: " + HINWEIS)
    return "\n".join(z), pro_tag, tage


# --------------------------------------------------------------------------------------------- Diagramme
def fig_pro_tag(df, tage):
    f, ax = plt.subplots(figsize=(12, 5))
    teile = [("Tag", ~df["nacht"] & ~df["stoerung"], C_OK), ("Tag - Stoerung", ~df["nacht"] & df["stoerung"], C_ST),
             ("Nacht", df["nacht"] & ~df["stoerung"], C_NACHT), ("Nacht - Stoerung", df["nacht"] & df["stoerung"], C_NACHT_ST)]
    unten = np.zeros(len(tage))
    for name, m, farbe in teile:
        v = df[m].groupby("tag").size().reindex(tage, fill_value=0).values
        ax.bar(tage, v, bottom=unten, color=farbe, label=name, width=0.8)
        unten += v
    ax.axhline(unten.mean(), color="k", ls="--", lw=1, label="Mittel %.1f/Tag" % unten.mean())
    ax.set_title("Ereignisse pro Tag"); ax.set_ylabel("Anzahl Ereignisse"); ax.legend(ncol=5, fontsize=8)
    ax.set_xticks(tage[:: max(1, len(tage) // 20)]); ax.set_xticklabels([WOCHENTAGE[d.dayofweek] + d.strftime("\n%d.%m.") for d in tage[:: max(1, len(tage) // 20)]], fontsize=8)
    ax.grid(axis="y", alpha=.3)
    return f


def fig_stunden(df):
    f, ax = plt.subplots(figsize=(12, 5))
    ok = df[~df["stoerung"]].groupby("stunde").size().reindex(range(24), fill_value=0)
    st = df[df["stoerung"]].groupby("stunde").size().reindex(range(24), fill_value=0)
    ax.bar(range(24), ok, color=C_OK, label="Ereignis"); ax.bar(range(24), st, bottom=ok, color=C_ST, label="Stoerung")
    ax.set_xticks(range(24)); ax.set_xticklabels(["%02d" % i for i in range(24)])
    ax.set_xlabel("Uhrzeit (Stunde)"); ax.set_ylabel("Anzahl Ereignisse (Summe aller Tage)")
    ax.set_title("Tagesgang: Ereignisse nach Uhrzeit"); ax.legend(); ax.grid(axis="y", alpha=.3)
    return f


def fig_heatmap(df, tage):
    pv = df.pivot_table(index="tag", columns="stunde", values="dt", aggfunc="count").reindex(index=tage, columns=range(24)).fillna(0)
    f, ax = plt.subplots(figsize=(12, max(4, 0.32 * len(tage) + 2)))
    im = ax.imshow(pv.values, aspect="auto", cmap="YlOrRd", interpolation="nearest")
    ax.set_xticks(range(24)); ax.set_xticklabels(["%02d" % i for i in range(24)])
    step = max(1, len(tage) // 40)
    ax.set_yticks(range(0, len(tage), step)); ax.set_yticklabels([WOCHENTAGE[tage[i].dayofweek] + tage[i].strftime(" %d.%m.") for i in range(0, len(tage), step)], fontsize=8)
    ax.set_xlabel("Uhrzeit (Stunde)"); ax.set_title("Ereignisse je Tag und Stunde (Muster, z. B. regelmaessige Zeiten)")
    cb = f.colorbar(im, ax=ax, label="Anzahl")
    cb.locator = matplotlib.ticker.MaxNLocator(integer=True); cb.update_ticks()
    return f


def fig_pegel(df):
    f, (a1, a2) = plt.subplots(1, 2, figsize=(12, 5))
    bins = np.arange(np.floor(df["lafmax_dba"].min() / 2) * 2, df["lafmax_dba"].max() + 4, 2)
    a1.hist([df.loc[~df["nacht"], "lafmax_dba"], df.loc[df["nacht"], "lafmax_dba"]], bins=bins, stacked=True, color=[C_OK, C_NACHT], label=["Tag", "Nacht"])
    for zr, farbe in ((False, C_OK), (True, C_NACHT)):
        g = df.loc[df["nacht"] == zr, "grenzwert_dba"].dropna()
        if len(g):
            a1.axvline(g.median(), color=farbe, ls="--"); a1.text(g.median(), a1.get_ylim()[1] * .95, " Grenzwert %s" % ("Nacht" if zr else "Tag"), color=farbe, rotation=90, va="top", fontsize=8)
    a1.set_xlabel("LAFmax [dB(A)]"); a1.set_ylabel("Anzahl"); a1.set_title("Pegelverteilung der Ereignisse"); a1.legend()
    s = np.sort(df["lafmax_dba"].values); a2.plot(s, 100.0 * (1 - np.arange(1, len(s) + 1) / len(s)), color="k")
    a2.set_xlabel("LAFmax [dB(A)]"); a2.set_ylabel("Anteil der Ereignisse darueber [%]"); a2.set_title("Ueberschreitungshaeufigkeit"); a2.grid(alpha=.3)
    return f


def fig_pegel_uhrzeit(df):
    f, ax = plt.subplots(figsize=(12, 5))
    x = df["stunde"] + df["minute"] / 60.0
    ax.scatter(x[~df["stoerung"]], df.loc[~df["stoerung"], "lafmax_dba"], s=18, c=C_OK, alpha=.6, label="Ereignis")
    ax.scatter(x[df["stoerung"]], df.loc[df["stoerung"], "lafmax_dba"], s=22, c=C_ST, alpha=.7, label="Stoerung")
    for zr, farbe, lbl in ((False, C_OK, "Grenzwert Tag"), (True, C_NACHT, "Grenzwert Nacht")):
        g = df.loc[df["nacht"] == zr, "grenzwert_dba"].dropna()
        if len(g):
            ax.axhline(g.median(), color=farbe, ls="--", lw=1, label=lbl)
    ax.set_xlim(0, 24); ax.set_xticks(range(0, 25)); ax.set_xlabel("Uhrzeit"); ax.set_ylabel("LAFmax [dB(A)]")
    ax.set_title("Pegel der Ereignisse ueber der Tageszeit (alle Tage uebereinander)"); ax.grid(alpha=.3); ax.legend(fontsize=8, ncol=4)
    return f


def fig_rhythmus(df):
    f, (a1, a2) = plt.subplots(1, 2, figsize=(12, 5))
    m = df.groupby("minute").size().reindex(range(60), fill_value=0)
    a1.bar(range(60), m.values, color=C_OK); a1.set_xticks(range(0, 60, 5)); a1.set_xlabel("Minute innerhalb der Stunde"); a1.set_ylabel("Anzahl Ereignisse")
    a1.set_title("Regelmaessigkeit: Minute der Stunde (Haeufung bei :00/:30?)"); a1.grid(axis="y", alpha=.3)
    if len(df) > 3:
        ab = (df["dt"].diff().dt.total_seconds().dropna() / 60.0)
        ab = ab[ab > 0]
        a2.hist(ab, bins=np.logspace(np.log10(max(ab.min(), 0.05)), np.log10(max(ab.max(), 1)), 40), color=C_OK)
        a2.set_xscale("log"); a2.set_xticks([t for t in (1, 2, 5, 10, 30, 60, 120, 300, 600, 1440) if ab.min() / 2 <= t <= ab.max() * 2])
        a2.xaxis.set_major_formatter(matplotlib.ticker.ScalarFormatter()); a2.xaxis.set_minor_formatter(matplotlib.ticker.NullFormatter()); a2.set_xlabel("Abstand zum vorigen Ereignis [min] (log)"); a2.set_ylabel("Anzahl"); a2.set_title("Zeitliche Abstaende"); a2.grid(alpha=.3)
    return f


def fig_naechte(df):
    nn = df[df["nacht"]].groupby("nachttag").agg(n=("dt", "size"), mx=("lafmax_dba", "max"), st=("stoerung", "sum"))
    if nn.empty:
        return None
    f, ax = plt.subplots(figsize=(12, 5))
    ax.bar(nn.index, nn["n"] - nn["st"], color=C_NACHT, label="Ereignisse"); ax.bar(nn.index, nn["st"], bottom=nn["n"] - nn["st"], color=C_NACHT_ST, label="Stoerungen")
    ax.set_ylabel("Anzahl in der Nacht"); ax.set_title("Naechte (Beginn am Abend): Ereignisse und hoechster Pegel")
    a2 = ax.twinx(); a2.plot(nn.index, nn["mx"], "o-", color="orange", label="max. LAFmax"); a2.set_ylabel("max. LAFmax [dB(A)]")
    g = df.loc[df["nacht"], "grenzwert_dba"].dropna()
    if len(g):
        a2.axhline(g.median(), color="orange", ls="--", lw=1)
    h1, l1 = ax.get_legend_handles_labels(); h2, l2 = a2.get_legend_handles_labels(); ax.legend(h1 + h2, l1 + l2, fontsize=8)
    ax.tick_params(axis="x", labelrotation=60, labelsize=8); ax.grid(axis="y", alpha=.3)
    return f


def fig_tagespegel(df, tage, nacht_start, nacht_ende):
    t_nacht = ((24 - nacht_start) + nacht_ende) * 3600.0
    t_tag = 86400.0 - t_nacht
    f, ax = plt.subplots(figsize=(12, 5))
    tag = [pegelsumme(df.loc[(df["tag"] == d) & ~df["nacht"], "lae_dba"], t_tag) for d in tage]
    nacht = [pegelsumme(df.loc[(df["nachttag"] == d) & df["nacht"], "lae_dba"], t_nacht) for d in tage]
    ax.plot(tage, tag, "o-", color=C_OK, label="Tag (%02d-%02d Uhr)" % (nacht_ende, nacht_start)); ax.plot(tage, nacht, "s-", color=C_NACHT, label="Nacht (%02d-%02d Uhr)" % (nacht_start, nacht_ende))
    ax.set_ylabel("Beitrag der Ereignisse zum Mittelungspegel [dB(A)]"); ax.set_title("Taeglicher Ereignispegel (aus LAE, ohne Hintergrundgeraeusch)")
    ax.legend(); ax.grid(alpha=.3); ax.tick_params(axis="x", labelrotation=60, labelsize=8)
    return f


def fig_dauer(df):
    f, ax = plt.subplots(figsize=(12, 5))
    d = df[df["aktiv_s"] > 0]
    ax.scatter(d.loc[~d["stoerung"], "aktiv_s"], d.loc[~d["stoerung"], "lafmax_dba"], s=18, c=C_OK, alpha=.6, label="Ereignis")
    ax.scatter(d.loc[d["stoerung"], "aktiv_s"], d.loc[d["stoerung"], "lafmax_dba"], s=22, c=C_ST, alpha=.7, label="Stoerung")
    ax.set_xscale("log"); ax.set_xlabel("aktive Dauer [s] (log)"); ax.set_ylabel("LAFmax [dB(A)]"); ax.set_title("Dauer und Pegel (kurze, laute Ereignisse = Knalle)")
    ax.grid(alpha=.3); ax.legend()
    return f


def fig_wochentage(df, tage):
    f, ax = plt.subplots(figsize=(8, 5))
    nwd = np.array([max(1, int(((tage.dayofweek) == i).sum())) for i in range(7)])
    tag = np.array([((df["wochentag"] == i) & ~df["nacht"]).sum() for i in range(7)]) / nwd
    nacht = np.array([((df["wochentag"] == i) & df["nacht"]).sum() for i in range(7)]) / nwd
    ax.bar(range(7), tag, color=C_OK, label="Tag"); ax.bar(range(7), nacht, bottom=tag, color=C_NACHT, label="Nacht")
    ax.set_xticks(range(7)); ax.set_xticklabels(WOCHENTAGE); ax.set_ylabel("Ereignisse pro Tag (Mittel)"); ax.set_title("Wochentage"); ax.legend(); ax.grid(axis="y", alpha=.3)
    return f


def fig_text(text):
    f = plt.figure(figsize=(11.7, 8.3)); f.text(0.04, 0.96, text, family="monospace", fontsize=8.2, va="top", wrap=True)
    return f


# --------------------------------------------------------------------------------------------- Hauptprogramm
def main():
    ap = argparse.ArgumentParser(description="Statistische Auswertung der Laermtracker-Ereignisse")
    ap.add_argument("pfad", nargs="?", help="Ordner mit EVENTS.CSV oder die CSV-Datei")
    ap.add_argument("--ausgabe"); ap.add_argument("--von"); ap.add_argument("--bis")
    ap.add_argument("--nacht-start", type=int, default=22); ap.add_argument("--nacht-ende", type=int, default=6)
    ap.add_argument("--kein-fenster", action="store_true")
    a = ap.parse_args()
    pfad = a.pfad
    if not pfad:
        try:
            import tkinter
            from tkinter import filedialog
            r = tkinter.Tk(); r.withdraw(); pfad = filedialog.askdirectory(title="Ordner mit EVENTS.CSV waehlen"); r.destroy()
        except Exception:
            pass
        if not pfad:
            ap.error("Bitte einen Ordner (mit EVENTS.CSV) angeben.")
    df, datei, n_roh, kaputt = lade(pfad, a.nacht_start, a.nacht_ende)
    if a.von:
        df = df[df["dt"] >= pd.Timestamp(a.von)]
    if a.bis:
        df = df[df["dt"] < pd.Timestamp(a.bis) + pd.Timedelta(days=1)]
    if len(df) == 0:
        raise SystemExit("Keine auswertbaren Ereignisse (Zeilen gelesen: %d, unlesbar: %d)." % (n_roh, kaputt))
    ausgabe = a.ausgabe or os.path.join(os.path.dirname(os.path.abspath(datei)), "auswertung")
    os.makedirs(ausgabe, exist_ok=True)

    text, pro_tag, tage = kennzahlen(df, a.nacht_start, a.nacht_ende)
    if kaputt:
        text += "\n\nACHTUNG: %d von %d Zeilen waren unlesbar und wurden ausgelassen (haeufig durch Excel: Zahlen wie 1,86 wurden in Datumswerte umgewandelt;\n" \
                "         CSV nicht mit Excel speichern oder das Original EVENTS.CSV verwenden)." % (kaputt, n_roh)
    print(text)
    with open(os.path.join(ausgabe, "kennzahlen.txt"), "w", encoding="utf-8") as fh:
        fh.write(text + "\n")

    tag_tab = pd.DataFrame({"ereignisse": pro_tag})
    g = df.groupby("tag")
    tag_tab["stoerungen"] = g["stoerung"].sum().reindex(tage, fill_value=0)
    tag_tab["lafmax_max"] = g["lafmax_dba"].max().reindex(tage)
    tag_tab["lafmax_median"] = g["lafmax_dba"].median().reindex(tage)
    tag_tab["nachts"] = df[df["nacht"]].groupby("tag").size().reindex(tage, fill_value=0)
    tag_tab.index.name = "datum"
    tag_tab.round(1).to_csv(os.path.join(ausgabe, "tages_statistik.csv"), sep=";", decimal=",", encoding="utf-8-sig")

    figs = [("01_ereignisse_pro_tag", fig_pro_tag(df, tage)), ("02_tagesgang_stunden", fig_stunden(df)), ("03_heatmap_tag_stunde", fig_heatmap(df, tage)),
            ("04_pegelverteilung", fig_pegel(df)), ("05_pegel_ueber_uhrzeit", fig_pegel_uhrzeit(df)), ("06_rhythmus_minute_abstaende", fig_rhythmus(df)),
            ("07_naechte", fig_naechte(df)), ("08_tagespegel_ereignisse", fig_tagespegel(df, tage, a.nacht_start, a.nacht_ende)),
            ("09_dauer_und_pegel", fig_dauer(df)), ("10_wochentage", fig_wochentage(df, tage))]
    with PdfPages(os.path.join(ausgabe, "auswertung.pdf")) as pdf:
        t = fig_text(text); pdf.savefig(t); plt.close(t)
        for name, f in figs:
            if f is None:
                continue
            f.text(0.5, 0.005, HINWEIS, ha="center", fontsize=6, color="gray")
            f.tight_layout(rect=(0, 0.02, 1, 1))
            f.savefig(os.path.join(ausgabe, name + ".png"), dpi=130)
            pdf.savefig(f); plt.close(f)
    print("\nErgebnisse in: %s" % ausgabe)
    if not a.kein_fenster and sys.platform.startswith("win"):
        os.startfile(ausgabe)                                           # pragma: no cover


if __name__ == "__main__":
    main()

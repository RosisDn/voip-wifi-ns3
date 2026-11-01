# voip-wifi-ns3
ns-3 simulation DTA 400
# VoIP over Wi-Fi with downloads (ns-3)

This is the code for my DTA400 project at University West. I wanted to find out how a VoIP call over Wi-Fi is affected when other users download at the same time.

The simulation has one access point, one server, one caller and 0, 2, 5, 10 or 20 users who download. I ran each case with and without voice priority (802.11e), 10 times each, so 100 runs in total.

## Files

- `voip-wifi.cc` – the simulation in ns-3
- `run_all.sh` – runs all cases without voice priority
- `run_qos.sh` – runs all cases with voice priority
- `analyze.py` – calculates the results and makes the graphs
- `voip-results.csv` – the results from all 100 runs
- `summary.csv` – mean values for each case
- `fig_*.png` – the graphs used in my report

## What you need

- ns-3 version 3.48 (I used Ubuntu in WSL on Windows)
- Python 3 with pandas and matplotlib:

```bash
sudo apt install -y python3-pandas python3-matplotlib
```

## How to run

**1. Copy the files to ns-3 and build**

```bash
cp voip-wifi.cc ~/ns-allinone-3.48/ns-3.48/scratch/
cp run_all.sh run_qos.sh analyze.py ~/ns-allinone-3.48/ns-3.48/
cd ~/ns-allinone-3.48/ns-3.48
chmod +x run_all.sh run_qos.sh
./ns3 build
```

**2. Test one run**

```bash
./ns3 run "voip-wifi --nDownloaders=0 --RngRun=1 --outFile=test.csv"
```

With no downloaders the delay should be about 1 ms and there should be no packet loss.

**3. Run all simulations**

I ran both scripts at the same time. It took about 4 hours on my laptop.

```bash
nohup ./run_all.sh > run.log 2>&1 &
sleep 60
nohup ./run_qos.sh > run_qos.log 2>&1 &
```

Keep the Ubuntu window open. When both are done, `run.log` and `run_qos.log` end with `Klart!`.

**4. Put the results together and make the graphs**

```bash
tail -n +2 voip-results-qos.csv >> voip-results.csv
python3 analyze.py
```

This creates `summary.csv` and the graphs.

## Run one case only

You can change the settings when you run the simulation, for example 10 downloaders with voice priority:

```bash
./ns3 run "voip-wifi --nDownloaders=10 --qos=1 --RngRun=1"
```

- `--nDownloaders` – number of users who download (default 0)
- `--qos` – 1 = voice priority on (default 0)
- `--RngRun` – run number / random seed (default 1)
- `--outFile` – file where the results are saved (default `voip-results.csv`)

## Where the code comes from

The simulation is based on two examples from ns-3.48: `third.cc` and `wifi-hidden-terminal.cc` (https://www.nsnam.org). The simulation, run and analysis scripts were made with help from the AI tool Claude. I tested the code, ran all the simulations and checked the results myself.


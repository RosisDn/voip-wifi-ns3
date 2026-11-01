/*
 * voip-wifi.cc
 *
 * Hur påverkas fördröjning, jitter och paketförlust för ett VoIP-samtal
 * över WiFi när fler användare laddar ner data samtidigt?
 *
 * Topologi:
 *
 *   [Samtalsanvändare]  ┐
 *   [Nedladdare 1]      │  WiFi 802.11n (5 GHz)        1 Gbit/s, 1 ms
 *   [Nedladdare 2]      ├─────────────────── [AP] ──────────────────── [Server]
 *     ...               │
 *   [Nedladdare N]      ┘
 *
 * - Samtalet: två UDP-flöden (G.711: 160 byte var 20:e ms),
 *   server -> samtalsanvändare (nedlänk, port 4000) och
 *   samtalsanvändare -> server (upplänk, port 4001).
 * - Nedladdningarna: ett TCP-flöde (BulkSend) per nedladdare, server -> station,
 *   port 5001, 5002, ...
 * - Mätning: FlowMonitor. En rad per VoIP-riktning skrivs till en CSV-fil.
 *
 * Kör (från ns-3-mappen):
 *   ./ns3 run "voip-wifi --nDownloaders=5 --RngRun=1"
 *
 * Skrivet för ns-3.44 och senare (testat mot 3.44, du har 3.48).
 */

#include "ns3/applications-module.h"
#include "ns3/core-module.h"
#include "ns3/flow-monitor-module.h"
#include "ns3/internet-module.h"
#include "ns3/mobility-module.h"
#include "ns3/network-module.h"
#include "ns3/point-to-point-module.h"
#include "ns3/wifi-module.h"

#include <cmath>
#include <fstream>
#include <iomanip>
#include <iostream>

using namespace ns3;

NS_LOG_COMPONENT_DEFINE("VoipWifi");

// 95:e percentilen av fördröjningen, uträknad ur FlowMonitors histogram (ms).
static double
DelayPercentileMs(const Histogram& h, double p)
{
    uint64_t total = 0;
    for (uint32_t i = 0; i < h.GetNBins(); ++i)
    {
        total += h.GetBinCount(i);
    }
    if (total == 0)
    {
        return 0.0;
    }
    uint64_t acc = 0;
    for (uint32_t i = 0; i < h.GetNBins(); ++i)
    {
        acc += h.GetBinCount(i);
        if (acc >= p * total)
        {
            return h.GetBinEnd(i) * 1000.0;
        }
    }
    return h.GetBinEnd(h.GetNBins() - 1) * 1000.0;
}

int
main(int argc, char* argv[])
{
    // ---------------------------------------------------------------
    // 1. Parametrar (kan ändras från kommandoraden)
    // ---------------------------------------------------------------
    uint32_t nDownloaders = 0;           // antal samtidiga nedladdare
    bool qos = false;                    // true = VoIP i röstkön (AC_VO)
    double distance = 5.0;               // meter från AP till varje station
    std::string dataMode = "HtMcs7";     // fast WiFi-hastighet
    double tcpStart = 1.0;               // s, nedladdningarna startar
    double voipStart = 10.0;             // s, samtalet startar (nätet redan belastat)
    double simStop = 70.0;               // s, all trafik stoppas
    std::string outFile = "voip-results.csv";
    bool verbose = false;                // skriv ut alla flöden (felsökning)

    CommandLine cmd(__FILE__);
    cmd.AddValue("nDownloaders", "Antal samtidiga TCP-nedladdare", nDownloaders);
    cmd.AddValue("qos", "Markera VoIP för röstkön AC_VO (0/1)", qos);
    cmd.AddValue("distance", "Avstånd AP-station i meter", distance);
    cmd.AddValue("dataMode", "WiFi-hastighet (t.ex. HtMcs7)", dataMode);
    cmd.AddValue("voipStart", "Starttid för samtalet (s)", voipStart);
    cmd.AddValue("simStop", "Stopptid för trafiken (s)", simStop);
    cmd.AddValue("outFile", "CSV-fil som resultatet läggs till i", outFile);
    cmd.AddValue("verbose", "Skriv ut statistik för alla flöden", verbose);
    cmd.Parse(argc, argv); // --RngRun=X fungerar automatiskt

    // Vanlig storlek på TCP-segment (1500 byte MTU - 52 byte huvuden)
    Config::SetDefault("ns3::TcpSocket::SegmentSize", UintegerValue(1448));

    // ---------------------------------------------------------------
    // 2. Noder
    // ---------------------------------------------------------------
    NodeContainer apNode;
    apNode.Create(1);
    NodeContainer staNodes; // nod 0 = samtalsanvändaren, 1..N = nedladdare
    staNodes.Create(1 + nDownloaders);
    NodeContainer serverNode;
    serverNode.Create(1);

    // ---------------------------------------------------------------
    // 3. Trådbunden länk AP <-> server (får aldrig vara flaskhals)
    // ---------------------------------------------------------------
    PointToPointHelper p2p;
    p2p.SetDeviceAttribute("DataRate", StringValue("1Gbps"));
    p2p.SetChannelAttribute("Delay", StringValue("1ms"));
    NetDeviceContainer p2pDevs = p2p.Install(apNode.Get(0), serverNode.Get(0));

    // ---------------------------------------------------------------
    // 4. WiFi 802.11n, 5 GHz, kanal 36, 20 MHz, fast hastighet
    // ---------------------------------------------------------------
    YansWifiChannelHelper channel = YansWifiChannelHelper::Default();
    YansWifiPhyHelper phy;
    phy.SetChannel(channel.Create());
    phy.Set("ChannelSettings", StringValue("{36, 20, BAND_5GHZ, 0}"));

    WifiHelper wifi;
    wifi.SetStandard(WIFI_STANDARD_80211n);
    wifi.SetRemoteStationManager("ns3::ConstantRateWifiManager",
                                 "DataMode",
                                 StringValue(dataMode),
                                 "ControlMode",
                                 StringValue("HtMcs0"));

    WifiMacHelper mac;
    Ssid ssid = Ssid("voip-test");
    mac.SetType("ns3::StaWifiMac", "Ssid", SsidValue(ssid));
    NetDeviceContainer staDevs = wifi.Install(phy, mac, staNodes);
    mac.SetType("ns3::ApWifiMac", "Ssid", SsidValue(ssid));
    NetDeviceContainer apDev = wifi.Install(phy, mac, apNode);

    // ---------------------------------------------------------------
    // 5. Fasta positioner: AP i mitten, stationer på en cirkel
    // ---------------------------------------------------------------
    Ptr<ListPositionAllocator> pos = CreateObject<ListPositionAllocator>();
    pos->Add(Vector(0.0, 0.0, 0.0)); // AP
    for (uint32_t i = 0; i < staNodes.GetN(); ++i)
    {
        double angle = 2.0 * M_PI * i / staNodes.GetN();
        pos->Add(Vector(distance * std::cos(angle), distance * std::sin(angle), 0.0));
    }
    MobilityHelper mobility;
    mobility.SetPositionAllocator(pos);
    mobility.SetMobilityModel("ns3::ConstantPositionMobilityModel");
    mobility.Install(apNode);   // tar första positionen
    mobility.Install(staNodes); // tar resten i ordning

    // ---------------------------------------------------------------
    // 6. IP-adresser och routing
    // ---------------------------------------------------------------
    InternetStackHelper stack;
    stack.Install(apNode);
    stack.Install(staNodes);
    stack.Install(serverNode);

    Ipv4AddressHelper address;
    address.SetBase("10.1.1.0", "255.255.255.0"); // WiFi-nätet
    Ipv4InterfaceContainer staIf = address.Assign(staDevs);
    address.Assign(apDev);
    address.SetBase("10.1.2.0", "255.255.255.0"); // trådbundna länken
    Ipv4InterfaceContainer p2pIf = address.Assign(p2pDevs);
    Ipv4GlobalRoutingHelper::PopulateRoutingTables();

    Ipv4Address callerAddr = staIf.GetAddress(0);
    Ipv4Address serverAddr = p2pIf.GetAddress(1);

    // ---------------------------------------------------------------
    // 7. Samtalet: UDP 160 byte var 20:e ms, båda riktningarna
    // ---------------------------------------------------------------
    const uint16_t voipDownPort = 4000; // server -> samtalsanvändare
    const uint16_t voipUpPort = 4001;   // samtalsanvändare -> server

    UdpServerHelper downSink(voipDownPort);
    ApplicationContainer voipSinks = downSink.Install(staNodes.Get(0));
    UdpServerHelper upSink(voipUpPort);
    voipSinks.Add(upSink.Install(serverNode.Get(0)));
    voipSinks.Start(Seconds(0.5));
    voipSinks.Stop(Seconds(simStop + 1.0));

    uint32_t maxPackets = static_cast<uint32_t>((simStop - voipStart) / 0.020) + 10;

    UdpClientHelper downClient(callerAddr, voipDownPort);
    UdpClientHelper upClient(serverAddr, voipUpPort);
    for (UdpClientHelper* c : {&downClient, &upClient})
    {
        c->SetAttribute("Interval", TimeValue(MilliSeconds(20)));
        c->SetAttribute("PacketSize", UintegerValue(160));
        c->SetAttribute("MaxPackets", UintegerValue(maxPackets));
        if (qos)
        {
            // ToS 0xc0 -> user priority 6 -> AC_VO (röstkön) i ns-3
            c->SetAttribute("Tos", UintegerValue(0xc0));
        }
    }
    ApplicationContainer voipSources = downClient.Install(serverNode.Get(0));
    voipSources.Add(upClient.Install(staNodes.Get(0)));
    voipSources.Start(Seconds(voipStart));
    voipSources.Stop(Seconds(simStop));

    // ---------------------------------------------------------------
    // 8. Nedladdningarna: en obegränsad TCP-överföring per nedladdare
    // ---------------------------------------------------------------
    Ptr<UniformRandomVariable> startJitter = CreateObject<UniformRandomVariable>();
    for (uint32_t i = 1; i <= nDownloaders; ++i)
    {
        uint16_t port = 5000 + i;

        PacketSinkHelper sink("ns3::TcpSocketFactory",
                              InetSocketAddress(Ipv4Address::GetAny(), port));
        ApplicationContainer s = sink.Install(staNodes.Get(i));
        s.Start(Seconds(0.5));
        s.Stop(Seconds(simStop + 1.0));

        BulkSendHelper src("ns3::TcpSocketFactory", InetSocketAddress(staIf.GetAddress(i), port));
        src.SetAttribute("MaxBytes", UintegerValue(0)); // 0 = obegränsat
        ApplicationContainer a = src.Install(serverNode.Get(0));
        // Små slumpvisa startskillnader så att inte alla startar exakt samtidigt
        a.Start(Seconds(tcpStart + startJitter->GetValue(0.0, 0.5)));
        a.Stop(Seconds(simStop));
    }

    // ---------------------------------------------------------------
    // 9. FlowMonitor och körning
    // ---------------------------------------------------------------
    FlowMonitorHelper flowmon;
    Ptr<FlowMonitor> monitor = flowmon.InstallAll();

    // En sekund extra så att paket som är på väg hinner fram
    Simulator::Stop(Seconds(simStop + 1.0));
    Simulator::Run();

    // ---------------------------------------------------------------
    // 10. Läs ut statistiken
    // ---------------------------------------------------------------
    monitor->CheckForLostPackets();
    Ptr<Ipv4FlowClassifier> classifier = DynamicCast<Ipv4FlowClassifier>(flowmon.GetClassifier());
    FlowMonitor::FlowStatsContainer stats = monitor->GetFlowStats();

    struct VoipResult
    {
        bool found = false;
        uint64_t tx = 0, rx = 0;
        double delayMs = 0, p95Ms = 0, jitterMs = 0, lossPct = 0;
    };

    VoipResult down, up;
    double tcpRxBytes = 0;

    for (const auto& [id, st] : stats)
    {
        Ipv4FlowClassifier::FiveTuple t = classifier->FindFlow(id);

        if (verbose)
        {
            std::cout << "Flöde " << id << " " << t.sourceAddress << ":" << t.sourcePort << " -> "
                      << t.destinationAddress << ":" << t.destinationPort
                      << " proto=" << int(t.protocol) << " tx=" << st.txPackets
                      << " rx=" << st.rxPackets << std::endl;
        }

        bool isUdp = (t.protocol == 17);
        bool isTcp = (t.protocol == 6);

        if (isUdp && (t.destinationPort == voipDownPort || t.destinationPort == voipUpPort))
        {
            VoipResult& r = (t.destinationPort == voipDownPort) ? down : up;
            r.found = true;
            r.tx = st.txPackets;
            r.rx = st.rxPackets;
            if (st.rxPackets > 0)
            {
                r.delayMs = st.delaySum.GetSeconds() * 1000.0 / st.rxPackets;
                r.p95Ms = DelayPercentileMs(st.delayHistogram, 0.95);
            }
            if (st.rxPackets > 1)
            {
                r.jitterMs = st.jitterSum.GetSeconds() * 1000.0 / (st.rxPackets - 1);
            }
            if (st.txPackets > 0)
            {
                r.lossPct = 100.0 * (double(st.txPackets) - double(st.rxPackets)) / st.txPackets;
            }
        }
        else if (isTcp && t.destinationPort > 5000 && t.destinationPort <= 5000 + nDownloaders)
        {
            tcpRxBytes += st.rxBytes; // bara dataflödena, inte kvittenserna
        }
    }

    double tcpMbps = tcpRxBytes * 8.0 / (simStop - tcpStart) / 1e6;

    // ---------------------------------------------------------------
    // 11. Skriv till CSV (rubrikrad om filen är ny)
    // ---------------------------------------------------------------
    bool newFile;
    {
        std::ifstream test(outFile);
        newFile = !test.good() || test.peek() == std::ifstream::traits_type::eof();
    }
    std::ofstream out(outFile, std::ios::app);
    if (newFile)
    {
        out << "n_downloaders,run,qos,direction,tx_packets,rx_packets,"
               "delay_ms,delay_p95_ms,jitter_ms,loss_pct,tcp_mbps\n";
    }

    uint64_t run = RngSeedManager::GetRun();
    auto writeRow = [&](const std::string& dir, const VoipResult& r) {
        out << nDownloaders << "," << run << "," << (qos ? 1 : 0) << "," << dir << "," << r.tx
            << "," << r.rx << "," << std::fixed << std::setprecision(3) << r.delayMs << ","
            << r.p95Ms << "," << r.jitterMs << "," << r.lossPct << "," << tcpMbps << "\n";
        std::cout << "N=" << nDownloaders << " run=" << run << " qos=" << qos << " " << dir
                  << ": delay=" << r.delayMs << " ms (p95 " << r.p95Ms << ")"
                  << " jitter=" << r.jitterMs << " ms loss=" << r.lossPct << " %"
                  << " tcp=" << tcpMbps << " Mbit/s" << std::endl;
    };

    if (!down.found || !up.found)
    {
        std::cerr << "VARNING: hittade inte båda VoIP-flödena. Kör med --verbose=1." << std::endl;
    }
    writeRow("down", down);
    writeRow("up", up);

    Simulator::Destroy();
    return 0;
}

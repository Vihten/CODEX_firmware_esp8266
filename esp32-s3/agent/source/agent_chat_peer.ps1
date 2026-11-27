# Windows PowerShell 5.1. The release builder embeds this file at script scope.
function Initialize-ChatPeerWire {
    if ('CodexLan.PeerWireV1' -as [type]) { return }
    Add-Type -ReferencedAssemblies System.dll,System.Core.dll,System.Security.dll,System.Web.Extensions.dll -TypeDefinition @'
using System;
using System.Collections.Generic;
using System.Diagnostics;
using System.IO;
using System.Linq;
using System.Net;
using System.Net.Security;
using System.Net.NetworkInformation;
using System.Net.Sockets;
using System.Security.Authentication;
using System.Security.Cryptography;
using System.Security.Cryptography.X509Certificates;
using System.Text;
using System.Text.RegularExpressions;
using System.Threading;
using System.Web.Script.Serialization;

namespace CodexLan {
public sealed class PeerWireV1 : IDisposable {
    const string Version = "LFPEER1";
    const int MaxFrame = 2048, DeadlineMs = 4500, MaxPeers = 64;
    static readonly UTF8Encoding Utf8 = new UTF8Encoding(false, true);
    readonly object gate = new object();
    readonly X509Certificate2 cert;
    readonly string root, id, name;
    readonly IPAddress bind;
    readonly IPAddress discoveryAddress;
    readonly int discoveryPort;
    readonly HashSet<Guid> physical = new HashSet<Guid>();
    readonly TcpListener listener;
    readonly UdpClient udp;
    readonly Thread discovery;
    readonly Thread[] receivers = new Thread[4];
    readonly HashSet<TcpClient> active = new HashSet<TcpClient>();
    volatile bool stopped;
    public string Pin { get; private set; }
    public int TcpPort { get; private set; }
    public string LastReceiveError { get; private set; }

    public sealed class Row {
        public string id, name, pin, address;
        public bool online;
    }
    sealed class Location {
        public string Id, Name, Pin;
        public IPEndPoint Endpoint;
        public long Stamp;
        public readonly Stopwatch Age = Stopwatch.StartNew();
    }
    sealed class Binding {
        public string Name, Pin;
    }
    readonly Dictionary<string, Binding> trust = new Dictionary<string, Binding>();
    readonly Dictionary<string, Location> locations = new Dictionary<string, Location>();
    // Discovery-thread only. Charge attempts even if the socket send fails.
    readonly Stopwatch replyClock = Stopwatch.StartNew();
    readonly Dictionary<string,long> lastReply = new Dictionary<string,long>();
    readonly Queue<long> recentReplies = new Queue<long>();
    bool ReserveReply(string pin) {
        long now = replyClock.ElapsedMilliseconds;
        foreach (string key in lastReply.Where(x=>now-x.Value >= 8000).Select(x=>x.Key).ToArray())
            lastReply.Remove(key);
        while (recentReplies.Count != 0 && now-recentReplies.Peek() >= 1000) recentReplies.Dequeue();
        if (lastReply.ContainsKey(pin) || recentReplies.Count >= 16) return false;
        recentReplies.Enqueue(now);
        lastReply[pin] = now;
        return true;
    }

    static void Require(bool ok, string message) {
        if (!ok) throw new InvalidDataException(message);
    }
    static bool ValidId(string s) { return s != null && Regex.IsMatch(s, @"\A[0-9A-F]{8}\z"); }
    static bool ValidName(string s) {
        return !String.IsNullOrWhiteSpace(s) && s.Length <= 128 && !s.Any(Char.IsControl);
    }
    static void CheckMessage(string uuid, string body) {
        Guid parsed;
        Require(Guid.TryParseExact(uuid, "D", out parsed) && parsed.ToString("D") == uuid, "Invalid message ID");
        Require(!String.IsNullOrEmpty(body) && body.Length <= 512 && Utf8.GetByteCount(body) <= 512 &&
            !body.Any(c => c != '\n' && Char.IsControl(c)), "Invalid message text");
    }
    static string Fingerprint(X509Certificate c) {
        using (var sha = SHA256.Create())
            return BitConverter.ToString(sha.ComputeHash(c.GetRawCertData())).Replace("-", "");
    }
    static bool Current(X509Certificate2 c) {
        return c.NotBefore.ToUniversalTime() <= DateTime.UtcNow && c.NotAfter.ToUniversalTime() > DateTime.UtcNow;
    }
    static string B64(string s) { return Convert.ToBase64String(Utf8.GetBytes(s)); }
    static string Decode(string s) { return Utf8.GetString(Convert.FromBase64String(s)); }
    public PeerWireV1(string mailbox, string client, string displayName, X509Certificate2 certificate,
                      IPAddress local, IPAddress target, int discoveryPort, int tcpPort, string[] adapterIds) {
        Require(ValidId(client) && ValidName(displayName), "Invalid local identity");
        root = Path.GetFullPath(mailbox);
        foreach (var sub in new [] {"inbox","seen"}) {
            var dir = new DirectoryInfo(Path.Combine(root,sub));
            Require(dir.Exists, "Initialize private mailbox first");
            for (; dir != null; dir = dir.Parent)
                Require((dir.Attributes & FileAttributes.ReparsePoint) == 0, "Reparse mailbox rejected");
        }
        id = client; name = displayName; cert = certificate;
        Require(cert.HasPrivateKey && Current(cert), "Invalid local certificate");
        Pin = Fingerprint(cert);
        bind = local;
        discoveryAddress = target;
        this.discoveryPort = discoveryPort;
        foreach (string adapter in adapterIds) physical.Add(Guid.Parse(adapter));
        listener = new TcpListener(local,tcpPort);
        udp = new UdpClient(AddressFamily.InterNetwork);
        try {
            listener.ExclusiveAddressUse = true;
            listener.Start(8);
            TcpPort = ((IPEndPoint)listener.LocalEndpoint).Port;
            udp.ExclusiveAddressUse = true;
            udp.Client.Bind(new IPEndPoint(local,discoveryPort));
            udp.EnableBroadcast = true;
            discovery = new Thread(Discover) { IsBackground = true };
            for (int i=0; i<receivers.Length; i++) {
                receivers[i] = new Thread(Serve) { IsBackground = true };
                receivers[i].Start();
            }
            discovery.Start();
        } catch { Dispose(); throw; }
    }

    public void Trust(string peer, string displayName, string pin) {
        Require(ValidId(peer) && peer != id && ValidName(displayName) && pin != null &&
            Regex.IsMatch(pin, @"\A[0-9A-F]{64}\z") && pin != Pin, "Invalid peer binding");
        lock (gate) {
            Require(!stopped && (trust.ContainsKey(peer) || trust.Count < MaxPeers), "Trust table unavailable");
            Require(!trust.Any(x => x.Key != peer && x.Value.Pin == pin), "Pin already bound");
            trust[peer] = new Binding {Name=displayName, Pin=pin};
        }
    }
    public void Remove(string peer) { lock (gate) trust.Remove(peer); }
    bool Fresh(Location loc) { return loc.Age.ElapsedMilliseconds < 16000; }
    Location Find(string peer, Binding b) {
        Location loc;
        return locations.TryGetValue(b.Pin, out loc) && loc.Id == peer && loc.Name == b.Name && Fresh(loc) ? loc : null;
    }
    public Row[] Roster() {
        lock (gate) return trust.Select(x => new Row {id=x.Key, name=x.Value.Name, pin=x.Value.Pin, online=Find(x.Key,x.Value)!=null}).ToArray();
    }
    public Row[] Candidates() {
        lock (gate) return locations.Values.Where(Fresh).Where(x => !trust.ContainsKey(x.Id) && locations.Values.Count(y => y.Id == x.Id && Fresh(y)) == 1).Select(x => new Row {
            id=x.Id, name=x.Name, pin=x.Pin, address=x.Endpoint.Address.ToString(), online=true
        }).ToArray();
    }

    byte[] Beacon(bool reply = false) {
        // A leading '+' marks a terminal reply inside the signature. Legacy LFPEER1
        // readers parse the same positive Int64 and accept it as ordinary presence.
        string data = String.Join("|", Version, id, B64(name), TcpPort.ToString(),
            (reply ? "+" : "") + DateTimeOffset.UtcNow.ToUnixTimeMilliseconds().ToString(),
            Convert.ToBase64String(cert.RawData));
        using (RSA key = cert.GetRSAPrivateKey()) {
            byte[] signature = key.SignData(Utf8.GetBytes(data),HashAlgorithmName.SHA256,RSASignaturePadding.Pkcs1);
            return Utf8.GetBytes(data + "|" + Convert.ToBase64String(signature));
        }
    }
    void AcceptBeacon(byte[] packet, int size, IPEndPoint source) {
        if (size > 4096) return;
        var data = Utf8.GetString(packet,0,size);
        var p = data.Split('|');
        int port; long stamp;
        if (p.Length != 7 || p[0] != Version || !ValidId(p[1]) || p[1] == id ||
            !Int32.TryParse(p[3],out port) || port < 1 || port > 65535 ||
            !Int64.TryParse(p[4],out stamp)) return;
        long now = DateTimeOffset.UtcNow.ToUnixTimeMilliseconds();
        if (stamp < now-15000 || stamp > now+15000) return;
        string displayName = Decode(p[2]);
        if (!ValidName(displayName)) return;
        var raw = Convert.FromBase64String(p[5]);
        if (X509Certificate2.GetCertContentType(raw) != X509ContentType.Cert) return;
        using (var remote = new X509Certificate2(raw)) {
            string pin = Fingerprint(remote);
            if (pin == Pin || !Current(remote)) return;
            bool terminal = p[4][0] == '+';
            lock (gate) {
                Location old;
                Binding b;
                if (locations.TryGetValue(pin,out old) && stamp <= old.Stamp) return;
                if (trust.Any(x=>x.Value.Pin == pin && x.Key != p[1])) return;
                if (trust.TryGetValue(p[1],out b) && (b.Pin != pin || b.Name != displayName)) return;
            }
            using (var key = remote.GetRSAPublicKey()) {
                if (key == null || key.KeySize != 2048 || !key.VerifyData(
                    Utf8.GetBytes(data.Substring(0,data.LastIndexOf('|'))),Convert.FromBase64String(p[6]),
                    HashAlgorithmName.SHA256,RSASignaturePadding.Pkcs1)) return;
            }
            bool reply;
            lock (gate) {
                Binding b;
                bool pinned = trust.TryGetValue(p[1],out b) && b.Pin == pin && b.Name == displayName;
                if (terminal && !pinned) return;
                foreach (var k in locations.Where(x=>!Fresh(x.Value)).Select(x=>x.Key).ToArray()) locations.Remove(k);
                if (!locations.ContainsKey(pin) && locations.Count >= MaxPeers) {
                    if (!trust.Values.Any(x=>x.Pin == pin)) return;
                    var victim = locations.Keys.FirstOrDefault(k=>!trust.Values.Any(x=>x.Pin == k));
                    if (victim == null) return;
                    locations.Remove(victim);
                }
                locations[pin] = new Location {Id=p[1],Name=displayName,Pin=pin,
                    Endpoint=new IPEndPoint(source.Address,port),Stamp=stamp};
                reply = !terminal && pinned && ReserveReply(pin);
            }
            if (reply) Announce(Beacon(true),source.Address);
        }
    }
    void Announce(byte[] packet, IPAddress replyTo = null) {
        if (discoveryAddress != null && (replyTo == null ||
            (IPAddress.IsLoopback(bind) && IPAddress.IsLoopback(replyTo) && !replyTo.Equals(bind)))) {
            udp.Send(packet,packet.Length,new IPEndPoint(replyTo ?? discoveryAddress,discoveryPort));
            return;
        }
        foreach (var adapter in NetworkInterface.GetAllNetworkInterfaces()) {
            Guid guid;
            if (adapter.OperationalStatus != OperationalStatus.Up || !Guid.TryParse(adapter.Id,out guid) ||
                !(physical.Contains(guid) || (discoveryAddress != null && !bind.Equals(IPAddress.Any)))) continue;
            foreach (var item in adapter.GetIPProperties().UnicastAddresses) {
                if (item.Address.AddressFamily != AddressFamily.InterNetwork || item.IPv4Mask == null ||
                    (!bind.Equals(IPAddress.Any) && !bind.Equals(item.Address))) continue;
                var ip = item.Address.GetAddressBytes();
                var mask = item.IPv4Mask.GetAddressBytes();
                bool lan = ip[0]==10 || (ip[0]==172 && ip[1]>=16 && ip[1]<=31) || (ip[0]==192 && ip[1]==168);
                if (!lan || mask[0]==0 || mask[3]>=254) continue;
                if (replyTo != null) {
                    var target = replyTo.GetAddressBytes();
                    bool sameSubnet = true, network = true, broadcast = true;
                    for (int i=0;i<4;i++) {
                        int host = target[i] & (255 ^ mask[i]);
                        sameSubnet &= ((target[i] ^ ip[i]) & mask[i]) == 0;
                        network &= host == 0;
                        broadcast &= host == (255 ^ mask[i]);
                    }
                    if (!sameSubnet || network || broadcast || replyTo.Equals(item.Address)) continue;
                }
                for (int i=0;i<4;i++) ip[i] |= (byte)~mask[i];
                try {
                    using (var outbound = new UdpClient(new IPEndPoint(item.Address,0))) {
                        outbound.EnableBroadcast = replyTo == null;
                        outbound.Send(packet,packet.Length,new IPEndPoint(replyTo ?? new IPAddress(ip),discoveryPort));
                        if (replyTo != null) return;
                    }
                } catch (SocketException) { }
            }
        }
    }
    void Discover() {
        var clock = Stopwatch.StartNew();
        long next = 0;
        var buffer = new byte[4097];
        while (!stopped) {
            try {
                if (clock.ElapsedMilliseconds >= next) {
                    next = clock.ElapsedMilliseconds + 4000;
                    var packet = Beacon();
                    Announce(packet);
                }
                if (!udp.Client.Poll(250000,SelectMode.SelectRead)) continue;
                EndPoint source = new IPEndPoint(IPAddress.Any,0);
                int n = udp.Client.ReceiveFrom(buffer,ref source);
                AcceptBeacon(buffer,n,(IPEndPoint)source);
            } catch (Exception) { if (stopped) break; }
            Thread.Sleep(15);
        }
    }
    static byte[] Exact(Stream stream, int n) {
        var data = new byte[n];
        for (int offset=0; offset<n;) {
            int read = stream.Read(data,offset,n-offset);
            if (read == 0) throw new EndOfStreamException();
            offset += read;
        }
        return data;
    }
    static string[] ReadFrame(Stream stream) {
        byte[] header = Exact(stream,2);
        int n = header[0]*256 + header[1];
        Require(n > 0 && n <= MaxFrame, "Invalid peer frame length");
        return Utf8.GetString(Exact(stream,n)).Split('|');
    }
    static void Frame(Stream stream, params string[] fields) {
        byte[] data = Utf8.GetBytes(String.Join("|",fields));
        Require(data.Length <= MaxFrame, "Oversized peer frame");
        stream.Write(new byte[]{(byte)(data.Length>>8),(byte)data.Length},0,2);
        stream.Write(data,0,data.Length);
        stream.Flush();
    }
    bool AcceptCert(X509Certificate candidate, string expected) {
        if (candidate == null) return false;
        using (var c = new X509Certificate2(candidate)) {
            string pin = Fingerprint(c);
            if (!Current(c) || pin == Pin) return false;
            lock (gate) return !stopped && trust.Any(x=>x.Value.Pin == pin && (expected == null || expected == x.Key));
        }
    }
    static void CheckTls(SslStream tls) {
        Require(tls.IsSigned && tls.IsEncrypted && tls.IsMutuallyAuthenticated && tls.RemoteCertificate != null,
                "Mutual TLS required");
    }
    string AssertBinding(string peer, string pin) {
        Binding b;
        Require(!stopped && peer != id && trust.TryGetValue(peer,out b) && trust[peer].Pin == pin,
                "Peer certificate binding rejected");
        return trust[peer].Name;
    }
    bool AlreadyDelivered(string path, string uuid, string sender, string pin, string body) {
        try {
            using (var f = new FileStream(path,FileMode.Open,FileAccess.Read,FileShare.Read|FileShare.Delete)) {
                Require(f.Length > 0 && f.Length <= 8192 &&
                    (File.GetAttributes(path) & FileAttributes.ReparsePoint) == 0, "Invalid receipt record");
                var json = Utf8.GetString(Exact(f,(int)f.Length)).TrimStart('\uFEFF');
                var record = new JavaScriptSerializer {MaxJsonLength=8192,RecursionLimit=4}
                    .Deserialize<Dictionary<string,object>>(json);
                Require((string)record["id"]==uuid && (string)record["sender_id"]==sender &&
                    record.ContainsKey("sender_fingerprint") && (string)record["sender_fingerprint"]==pin && (string)record["text"]==body, "Message ID conflict");
                return true;
            }
        } catch (FileNotFoundException) { return false; }
    }
    void Deliver(string uuid, string sender, string senderName, string pin, string body) {
        string inbox = Path.Combine(root,"inbox");
        string path = Path.Combine(inbox,uuid+".json");
        string seen = Path.Combine(root,"seen",uuid+".json");
        // The tray moves inbox to seen in this direction. Hold an open read handle across the move.
        if (AlreadyDelivered(path,uuid,sender,pin,body) || AlreadyDelivered(seen,uuid,sender,pin,body)) return;
        Require(Directory.EnumerateFiles(inbox,"*.json").Take(64).Count()<64,"Inbox full");
        byte[] data = Utf8.GetBytes(new JavaScriptSerializer().Serialize(new {
            id=uuid, sender_id=sender, sender_name=senderName, sender_fingerprint=pin, text=body
        }));
        string temp = Path.Combine(inbox,Guid.NewGuid().ToString("N")+".tmp");
        try {
            using (var f = new FileStream(temp,FileMode.CreateNew,FileAccess.Write,FileShare.None)) {
                f.Write(data,0,data.Length);
                f.Flush(true);
            }
            File.Move(temp,path);
        } finally { File.Delete(temp); }
    }
    void Receive(TcpClient client) {
        using (var tls = new SslStream(client.GetStream(),false,
                (s,c,ch,e)=>AcceptCert(c,null))) {
            tls.AuthenticateAsServer(cert,true,SslProtocols.Tls12,false);
            CheckTls(tls);
            var p = ReadFrame(tls);
            Require(p.Length==7 && p[0]==Version && p[1]=="SEND" && ValidId(p[2]) && p[3]==id, "Invalid recipient or frame");
            var sender = p[2];
            var senderName = Decode(p[5]);
            var body = Decode(p[6]);
            CheckMessage(p[4],body);
            lock (gate) {
                string pin = Fingerprint(tls.RemoteCertificate);
                string trustedName = AssertBinding(sender,pin);
                Require(trustedName == senderName, "Sender name mismatch");
                Deliver(p[4],sender,trustedName,pin,body);
            }
            Frame(tls,Version,"ACK",id,sender,p[4]);
        }
    }
    static void Close(object obj) {
        try { ((TcpClient)obj).Close(); } catch (ObjectDisposedException) { }
    }
    void Serve() {
        while (!stopped) {
            try {
                using (var client = listener.AcceptTcpClient()) {
                    lock (gate) {
                        if (stopped) return;
                        active.Add(client);
                    }
                    try {
                        using (var deadline = new Timer(Close,client,DeadlineMs,Timeout.Infinite)) Receive(client);
                    } finally { lock (gate) active.Remove(client); }
                }
            } catch (Exception ex) {
                if (stopped) break;
                LastReceiveError = ex.GetType().Name + ": " + ex.Message;
            }
        }
    }
    public string Send(string recipient,string uuid,string body) {
        CheckMessage(uuid,body);
        Binding b;
        IPEndPoint endpoint;
        using (var client = new TcpClient(new IPEndPoint(bind,0))) {
            lock (gate) {
                Require(!stopped && recipient != id && trust.ContainsKey(recipient), "Untrusted recipient");
                b = trust[recipient];
                Location loc = Find(recipient,b);
                Require(loc != null,"Peer offline");
                endpoint = loc.Endpoint;
                active.Add(client);
            }
            try {
                using (var deadline = new Timer(Close,client,DeadlineMs,Timeout.Infinite)) {
                    client.Connect(endpoint);
                    using (var tls = new SslStream(client.GetStream(),false,
                        (s,c,ch,e)=>AcceptCert(c,recipient), (s,h,cs,r,issuers)=>cert)) {
                        tls.AuthenticateAsClient("codex-peer-"+recipient,new X509CertificateCollection {cert},SslProtocols.Tls12,false);
                        CheckTls(tls);
                        lock (gate) AssertBinding(recipient,Fingerprint(tls.RemoteCertificate));
                        Frame(tls,Version,"SEND",id,recipient,uuid,B64(name),B64(body));
                        var ack = ReadFrame(tls);
                        Require(ack.Length==5 && ack[0]==Version && ack[1]=="ACK" &&
                            ack[2]==recipient && ack[3]==id && ack[4]==uuid, "Invalid peer receipt");
                        return uuid;
                    }
                }
            } finally { lock (gate) active.Remove(client); }
        }
    }
    public void Dispose() {
        lock (gate) {
            if (stopped) return;
            stopped = true;
            foreach (var client in active) Close(client);
        }
        listener.Stop();
        udp.Close();
        foreach (Thread receiver in receivers) if (receiver != null && receiver.IsAlive) receiver.Join();
        if (discovery != null && discovery.IsAlive) discovery.Join();
    }
}
}
'@ -ErrorAction Stop
}

function Get-ChatPeerCertificate([string]$Client) {
    if ($Client -cnotmatch '\A[0-9A-F]{8}\z') { throw 'Invalid peer client ID' }
    Import-Module (Join-Path $PSHOME 'Modules/Microsoft.PowerShell.Security/Microsoft.PowerShell.Security.psd1') -ErrorAction Stop
    $subject = "CN=Codex-LAN-Peer-v1-$Client"
    $mutex = [Threading.Mutex]::new($false,"Local\CodexLanPeerCertificate-$Client")
    $held = $false
    try {
        try { $held = $mutex.WaitOne(10000) } catch [Threading.AbandonedMutexException] { $held = $true }
        if (-not $held) { throw 'Peer certificate initialization busy' }
        $found = @(Get-ChildItem Cert:\CurrentUser\My -ErrorAction Stop | Where-Object { $_.Subject -ceq $subject })
        if ($found.Count -gt 1) { throw 'Ambiguous local peer certificate' }
        if ($found.Count) { return $found[0] }
        Import-Module (Join-Path $PSHOME 'Modules/PKI/PKI.psd1') -ErrorAction Stop
        New-SelfSignedCertificate -Subject $subject -CertStoreLocation Cert:\CurrentUser\My -Type Custom -KeyAlgorithm RSA -KeyLength 2048 -HashAlgorithm SHA256 -KeyExportPolicy NonExportable -KeyUsage DigitalSignature,KeyEncipherment -TextExtension '2.5.29.37={text}1.3.6.1.5.5.7.3.1,1.3.6.1.5.5.7.3.2' -NotAfter ([datetime]::Now.AddYears(3)) -ErrorAction Stop
    } finally {
        if ($held) { $mutex.ReleaseMutex() }
        $mutex.Dispose()
    }
}

function Start-ChatPeerRuntime {
    param([string]$Root,[string]$Client,[string]$Name,[ipaddress]$BindAddress='0.0.0.0',
          [ipaddress]$DiscoveryAddress,[ValidateRange(1,65535)][int]$DiscoveryPort=4214,
          [ValidateRange(0,65535)][int]$TcpPort=4215)
    Initialize-ChatPeerWire
    $certificate = Get-ChatPeerCertificate $Client
    $adapterIds = @()
    if (-not $DiscoveryAddress) {
        Import-Module (Join-Path $PSHOME 'Modules/NetAdapter/NetAdapter.psd1') -ErrorAction Stop
        $adapterIds = @(Get-NetAdapter -Physical -ErrorAction Stop | ForEach-Object { [string]$_.InterfaceGuid })
    }
    [CodexLan.PeerWireV1]::new($Root,$Client,$Name,$certificate,$BindAddress,$DiscoveryAddress,$DiscoveryPort,$TcpPort,[string[]]$adapterIds)
}
function Stop-ChatPeerRuntime($Runtime) { if ($Runtime) { $Runtime.Dispose() } }
function Set-ChatPeerTrust($Runtime,$PeerId,$Name,$Pin) { $Runtime.Trust($PeerId,$Name,$Pin) }
function Remove-ChatPeerTrust($Runtime,$PeerId) { $Runtime.Remove($PeerId) }
function Get-ChatPeerRoster($Runtime) { $Runtime.Roster() }
function Get-ChatPeerCandidates($Runtime) { $Runtime.Candidates() }
function Send-ChatPeerMessage($Runtime,$RecipientId,$Id,$Text) { $Runtime.Send($RecipientId,$Id,$Text) }

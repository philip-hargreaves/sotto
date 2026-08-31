using System.Net;
using System.Net.Sockets;
using Ambient.Fetch;

// fetch <registryDir> <storeRoot>   install every pack that does not verify
// pack <sourceDir> <id> <baseUrl> <target> <uploadDir>   prepare a release
if (args.Length >= 3 && args[0] == "fetch")
{
    var packs = Directory.EnumerateFiles(args[1], "*.json").Select(Pack.Load).ToList();
    var total = packs.Sum(p => p.TotalBytes);
    var needed = total + Packer.ShardBytes;  // one shard of transient on top
    var free = new DriveInfo(Path.GetFullPath(args[2])).AvailableFreeSpace;
    if (free < needed)
    {
        Console.Error.WriteLine(
            $"not enough disk: {free / (1 << 30)} GiB free, {needed / (1 << 30)} GiB needed");
        return 1;
    }

    var fetcher = new Fetcher(MakeClient(), Console.WriteLine);
    var failures = 0;
    foreach (var pack in packs)
    {
        try
        {
            await fetcher.InstallAsync(pack, args[2]);
        }
        catch (Exception e)
        {
            Console.Error.WriteLine($"{pack.Id}: FAILED ({e.Message}); re-run to resume");
            failures++;
        }
    }

    Console.WriteLine(failures == 0 ? "all packs verified" : $"{failures} pack(s) failed");
    return failures == 0 ? 0 : 1;
}

if (args.Length == 6 && args[0] == "pack")
{
    var pack = Packer.Pack(args[1], args[2], args[3], args[4], args[5]);
    Console.WriteLine(
        $"packed {pack.Id}: {pack.Shards.Values.Sum(s => s.Count)} assets, "
        + $"{pack.TotalBytes / (1 << 20)} MiB; upload the assets, commit {pack.Id}.json");
    return 0;
}

Console.Error.WriteLine("usage: fetch <registryDir> <storeRoot>"
    + " | pack <sourceDir> <id> <baseUrl> <target> <uploadDir>");
return 2;

// IPv4 first: broken IPv6 routes otherwise stall every request until timeout
static HttpClient MakeClient()
{
    var handler = new SocketsHttpHandler
    {
        AllowAutoRedirect = true,
        ConnectCallback = async (context, ct) =>
        {
            var addresses = await Dns.GetHostAddressesAsync(context.DnsEndPoint.Host, ct)
                .ConfigureAwait(false);
            var ordered = addresses.OrderBy(a => a.AddressFamily == AddressFamily.InterNetwork
                ? 0 : 1).ToList();
            Exception? last = null;
            foreach (var address in ordered)
            {
                var socket = new Socket(address.AddressFamily, SocketType.Stream,
                    ProtocolType.Tcp)
                { NoDelay = true };
                try
                {
                    await socket.ConnectAsync(address, context.DnsEndPoint.Port, ct)
                        .ConfigureAwait(false);
                    return new NetworkStream(socket, ownsSocket: true);
                }
                catch (Exception e)
                {
                    socket.Dispose();
                    last = e;
                }
            }

            throw last ?? new SocketException((int)SocketError.HostNotFound);
        },
    };
    return new HttpClient(handler) { Timeout = TimeSpan.FromMinutes(30) };
}

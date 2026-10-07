package com.orailnoor.droiddesk.runtime

import android.content.Context
import android.net.ConnectivityManager
import android.net.LinkProperties
import android.net.Network
import android.net.NetworkCapabilities
import android.net.NetworkRequest
import android.net.LocalServerSocket
import android.net.LocalSocket
import android.system.Os
import android.system.OsConstants
import android.system.StructPollfd
import android.util.Log
import java.io.DataInputStream
import java.io.InputStream
import java.io.OutputStream
import java.net.ConnectException
import java.net.DatagramPacket
import java.net.DatagramSocket
import java.net.Inet4Address
import java.net.InetAddress
import java.net.InetSocketAddress
import java.net.NoRouteToHostException
import java.net.ServerSocket
import java.net.Socket
import java.net.SocketTimeoutException
import java.util.concurrent.ConcurrentHashMap
import kotlin.concurrent.thread

/**
 * Lets Linux reach networks Android does not route to, such as an Ethernet dock in a
 * VLAN without internet while the mobile data stay the default network. Without root
 * Linux cannot pick the interface itself (the kernel takes the default network's
 * source address), but the app can bind its sockets to any network it sees.
 *
 * For each network switched on ("enable") a SOCKS5 server on 127.0.0.1 connects
 * through that network only (Ethernet 1081, Wi-Fi 1082, others from 1083). The router
 * on 1080 picks the network by the rules (subnet -> interface, longest prefix wins) and
 * uses the default network for everything else; droiddesk-net run and the PAC file
 * use it. TCP only: UDP and fixed ports go through "forward".
 *
 * Protocol, one line per request on the abstract socket "droiddesk.net":
 *   status              -> "net\t<iface>\t<transport>\t<addresses>\t<validated>\t<default>\t<enabled>\t<port>\t<state>",
 *                          "router\t<port>\t<state>", "rule\t<cidr>\t<iface>", then an empty line
 *   enable <iface> | disable <iface>         -> "ok" / "err <reason>"
 *   rule-add <cidr> <iface> | rule-del <cidr> -> "ok" / "err <reason>"
 *   ping <iface|auto> <host> [count]          -> streams result lines, then "done"
 *   forward <tcp|udp> <local port> <iface|auto> <host> <port>
 *                       -> "ok <local port>", forwards until the client closes the socket
 */
object NetBridge {
    private const val TAG = "NetBridge"
    private const val SOCKET_NAME = "droiddesk.net"
    private const val PREFS = "droiddesk_net"
    const val ROUTER_PORT = 1080
    private const val CONNECT_TIMEOUT_MS = 10_000

    private class Info(
        val network: Network,
        val iface: String,
        val transport: String,
        val addresses: List<String>,
        val validated: Boolean,
    )

    private sealed class Route {
        object Default : Route()
        class Via(val network: Network) : Route()
        class Unreachable(val reason: String) : Route()
    }

    private class Rule(val cidr: String, val base: Int, val prefix: Int, val iface: String) {
        fun matches(address: InetAddress): Boolean {
            if (address !is Inet4Address) return false
            val value = java.nio.ByteBuffer.wrap(address.address).int
            val mask = if (prefix == 0) 0 else -1 shl (32 - prefix)
            return (value and mask) == (base and mask)
        }
    }

    @Volatile private var server: LocalServerSocket? = null
    @Volatile private var appContext: Context? = null
    private var callback: ConnectivityManager.NetworkCallback? = null
    private val networks = ConcurrentHashMap<Network, Info>()
    private val proxies = ConcurrentHashMap<Int, ServerSocket>()

    fun start(context: Context) {
        if (server != null) return
        synchronized(this) {
            if (server != null) return
            try {
                appContext = context.applicationContext
                val socket = LocalServerSocket(SOCKET_NAME)
                server = socket
                watchNetworks(context.applicationContext)
                thread(name = "net-bridge", isDaemon = true) { serve(socket) }
                applyProxies()
                Log.i(TAG, "Network bridge started")
            } catch (error: Exception) {
                Log.e(TAG, "Could not start the network bridge", error)
            }
        }
    }

    fun stop() {
        synchronized(this) {
            val socket = server ?: return
            server = null
            runCatching { socket.close() }
        }
        proxies.values.forEach { runCatching { it.close() } }
        proxies.clear()
        appContext?.let { context ->
            callback?.let { runCatching { context.getSystemService(ConnectivityManager::class.java).unregisterNetworkCallback(it) } }
        }
        callback = null
        networks.clear()
    }

    // ── Networks ──

    private fun watchNetworks(context: Context) {
        val connectivity = context.getSystemService(ConnectivityManager::class.java)
        // Without INTERNET in the request it also sees networks that have none, like a
        // dock in a closed VLAN.
        val request = NetworkRequest.Builder()
            .removeCapability(NetworkCapabilities.NET_CAPABILITY_INTERNET)
            .removeCapability(NetworkCapabilities.NET_CAPABILITY_NOT_VPN)
            .build()
        val watcher = object : ConnectivityManager.NetworkCallback() {
            override fun onCapabilitiesChanged(network: Network, capabilities: NetworkCapabilities) = update(network)
            override fun onLinkPropertiesChanged(network: Network, linkProperties: LinkProperties) = update(network)
            override fun onLost(network: Network) {
                networks.remove(network)
            }

            private fun update(network: Network) {
                val capabilities = connectivity.getNetworkCapabilities(network) ?: return
                val link = connectivity.getLinkProperties(network) ?: return
                val iface = link.interfaceName ?: return
                networks[network] = Info(
                    network = network,
                    iface = iface,
                    transport = transportOf(capabilities),
                    addresses = link.linkAddresses.map { it.toString() },
                    validated = capabilities.hasCapability(NetworkCapabilities.NET_CAPABILITY_VALIDATED),
                )
            }
        }
        connectivity.registerNetworkCallback(request, watcher)
        callback = watcher
    }

    private fun transportOf(capabilities: NetworkCapabilities): String = when {
        capabilities.hasTransport(NetworkCapabilities.TRANSPORT_VPN) -> "vpn"
        capabilities.hasTransport(NetworkCapabilities.TRANSPORT_ETHERNET) -> "ethernet"
        capabilities.hasTransport(NetworkCapabilities.TRANSPORT_WIFI) -> "wifi"
        capabilities.hasTransport(NetworkCapabilities.TRANSPORT_CELLULAR) -> "mobile"
        capabilities.hasTransport(NetworkCapabilities.TRANSPORT_USB) -> "usb"
        else -> "other"
    }

    private fun networkOf(iface: String): Network? = networks.values.firstOrNull { it.iface == iface }?.network

    private fun defaultNetwork(): Network? =
        appContext?.getSystemService(ConnectivityManager::class.java)?.activeNetwork

    // ── Settings ──

    private fun prefs() = appContext!!.getSharedPreferences(PREFS, Context.MODE_PRIVATE)

    private fun enabledIfaces(): Set<String> = prefs().getStringSet("enabled", emptySet()).orEmpty()

    private fun rules(): List<Rule> = prefs().getString("rules", "").orEmpty().lines()
        .mapNotNull { line -> line.split(' ').takeIf { it.size == 2 }?.let { parseRule(it[0], it[1]) } }

    private fun parseRule(cidr: String, iface: String): Rule? {
        val parts = cidr.split('/')
        val prefix = parts.getOrNull(1)?.toIntOrNull() ?: 32
        if (parts.size > 2 || prefix !in 0..32) return null
        val address = runCatching { InetAddress.getByName(parts[0]) }.getOrNull() as? Inet4Address ?: return null
        if (!parts[0].matches(Regex("[0-9.]+"))) return null
        val base = java.nio.ByteBuffer.wrap(address.address).int
        val mask = if (prefix == 0) 0 else -1 shl (32 - prefix)
        val normalized = InetAddress.getByAddress(java.nio.ByteBuffer.allocate(4).putInt(base and mask).array())
        return Rule("${normalized.hostAddress}/$prefix", base and mask, prefix, iface)
    }

    private fun saveRules(rules: List<Rule>) {
        prefs().edit().putString("rules", rules.joinToString("\n") { "${it.cidr} ${it.iface}" }).apply()
    }

    /** Ethernet 1081, Wi-Fi 1082, anything else from 1083; kept per interface. */
    private fun portOf(iface: String): Int {
        val prefs = prefs()
        prefs.getInt("port_$iface", 0).takeIf { it > 0 }?.let { return it }
        val transport = networks.values.firstOrNull { it.iface == iface }?.transport
        val used = prefs.all.filterKeys { it.startsWith("port_") }.values.filterIsInstance<Int>().toSet()
        val port = when {
            transport == "ethernet" && 1081 !in used -> 1081
            transport == "wifi" && 1082 !in used -> 1082
            else -> generateSequence(1083) { it + 1 }.first { it !in used }
        }
        prefs.edit().putInt("port_$iface", port).apply()
        return port
    }

    // ── SOCKS servers ──

    /** Runs the router and one server per switched-on network; nothing while none is on. */
    @Synchronized
    private fun applyProxies() {
        val enabled = enabledIfaces()
        val wanted = if (enabled.isEmpty()) emptyMap() else
            enabled.associateBy(::portOf) + (ROUTER_PORT to "")
        proxies.keys.filter { it !in wanted }.forEach { port -> proxies.remove(port)?.let { runCatching { it.close() } } }
        for ((port, iface) in wanted) {
            if (port in proxies) continue
            try {
                val listener = ServerSocket(port, 50, InetAddress.getByName("127.0.0.1"))
                proxies[port] = listener
                thread(name = "net-socks-$port", isDaemon = true) {
                    acceptLoop(listener) { client -> socks(client, iface.ifEmpty { null }) }
                }
                Log.i(TAG, "SOCKS on 127.0.0.1:$port for ${iface.ifEmpty { "router" }}")
            } catch (error: Exception) {
                Log.w(TAG, "SOCKS port $port is busy: ${error.message}")
            }
        }
    }

    private fun acceptLoop(listener: ServerSocket, handle: (Socket) -> Unit) {
        while (!listener.isClosed) {
            val client = try {
                listener.accept()
            } catch (error: Exception) {
                break
            }
            thread(name = "net-client", isDaemon = true) {
                try {
                    handle(client)
                } catch (error: Exception) {
                    Log.d(TAG, "Connection ended: ${error.message}")
                } finally {
                    runCatching { client.close() }
                }
            }
        }
    }

    /** iface null: the router, which picks the network by the rules. */
    private fun routeFor(iface: String?, address: InetAddress): Route {
        val target = iface ?: rules().filter { it.matches(address) }.maxByOrNull { it.prefix }?.iface
            ?: return Route.Default
        if (target !in enabledIfaces()) return Route.Unreachable("$target is not switched on for Linux")
        val network = networkOf(target) ?: return Route.Unreachable("$target is not connected")
        return Route.Via(network)
    }

    private fun resolve(iface: String?, host: String): InetAddress {
        // Literal addresses need no lookup; names go through the chosen network's DNS
        // (the default network's for the router).
        if (host.matches(Regex("[0-9.]+")) || host.contains(':')) return InetAddress.getByName(host)
        val network = iface?.let(::networkOf)
        val all = network?.getAllByName(host) ?: InetAddress.getAllByName(host)
        return all.firstOrNull { it is Inet4Address } ?: all.first()
    }

    private fun connect(route: Route, address: InetAddress, port: Int): Socket {
        val socket = when (route) {
            is Route.Via -> route.network.socketFactory.createSocket()
            is Route.Default -> Socket()
            is Route.Unreachable -> throw NoRouteToHostException(route.reason)
        }
        try {
            socket.connect(InetSocketAddress(address, port), CONNECT_TIMEOUT_MS)
        } catch (error: Exception) {
            runCatching { socket.close() }
            throw error
        }
        return socket
    }

    private fun socks(client: Socket, iface: String?) {
        client.soTimeout = 30_000
        val input = DataInputStream(client.getInputStream())
        val output = client.getOutputStream()
        if (input.readUnsignedByte() != 5) return
        val methods = ByteArray(input.readUnsignedByte()).also(input::readFully)
        if (0.toByte() !in methods) {
            output.write(byteArrayOf(5, 0xFF.toByte()))
            return
        }
        output.write(byteArrayOf(5, 0))
        input.readUnsignedByte()
        val command = input.readUnsignedByte()
        input.readUnsignedByte()
        val host = when (input.readUnsignedByte()) {
            1 -> InetAddress.getByAddress(ByteArray(4).also(input::readFully)).hostAddress!!
            3 -> String(ByteArray(input.readUnsignedByte()).also(input::readFully), Charsets.US_ASCII)
            4 -> InetAddress.getByAddress(ByteArray(16).also(input::readFully)).hostAddress!!
            else -> return reply(output, 8)
        }
        val port = input.readUnsignedShort()
        if (command != 1) return reply(output, 7)
        val upstream = try {
            val address = resolve(iface, host)
            connect(routeFor(iface, address), address, port)
        } catch (error: Exception) {
            Log.i(TAG, "SOCKS ${iface ?: "router"} -> $host:$port failed: ${error.message}")
            return reply(output, when (error) {
                is ConnectException -> 5
                is NoRouteToHostException, is SocketTimeoutException -> 4
                is java.net.UnknownHostException -> 4
                else -> 1
            })
        }
        upstream.use {
            reply(output, 0)
            client.soTimeout = 0
            pipe(client, upstream)
        }
    }

    private fun reply(output: OutputStream, code: Int) {
        output.write(byteArrayOf(5, code.toByte(), 0, 1, 0, 0, 0, 0, 0, 0))
        output.flush()
    }

    /** Copies both ways until either side closes. */
    private fun pipe(a: Socket, b: Socket) {
        val back = thread(name = "net-pipe", isDaemon = true) {
            copy(b.getInputStream(), a.getOutputStream())
            runCatching { a.shutdownOutput() }
        }
        copy(a.getInputStream(), b.getOutputStream())
        runCatching { b.shutdownOutput() }
        back.join()
    }

    private fun copy(input: InputStream, output: OutputStream) {
        val buffer = ByteArray(16 * 1024)
        try {
            while (true) {
                val read = input.read(buffer)
                if (read < 0) break
                output.write(buffer, 0, read)
                output.flush()
            }
        } catch (error: Exception) {
            // The other side closed.
        }
    }

    // ── Control socket ──

    private fun serve(socket: LocalServerSocket) {
        while (server === socket) {
            val client = try {
                socket.accept()
            } catch (error: Exception) {
                if (server === socket) Log.w(TAG, "Network bridge accept failed", error)
                continue
            }
            if (client.peerCredentials.uid != android.os.Process.myUid()) {
                Log.w(TAG, "Rejected network request from uid ${client.peerCredentials.uid}")
                runCatching { client.close() }
                continue
            }
            thread(name = "net-bridge-client", isDaemon = true) {
                try {
                    handle(client)
                } catch (error: Exception) {
                    Log.d(TAG, "Network request ended: ${error.message}")
                } finally {
                    runCatching { client.close() }
                }
            }
        }
    }

    private fun handle(client: LocalSocket) {
        val reader = client.inputStream.bufferedReader()
        val output = client.outputStream
        fun say(line: String) {
            output.write((line + "\n").toByteArray())
            output.flush()
        }
        val args = reader.readLine()?.trim().orEmpty().split(Regex("\\s+")).filter { it.isNotEmpty() }
        when (args.firstOrNull()) {
            "status" -> {
                val enabled = enabledIfaces()
                val default = defaultNetwork()
                val seen = mutableSetOf<String>()
                networks.values.sortedBy { it.iface }.forEach { info ->
                    seen += info.iface
                    val on = info.iface in enabled
                    val port = if (on) portOf(info.iface) else 0
                    say(listOf(
                        "net", info.iface, info.transport, info.addresses.joinToString(","),
                        if (info.validated) "1" else "0", if (info.network == default) "1" else "0",
                        if (on) "1" else "0", port.toString(), proxyState(on, port),
                    ).joinToString("\t"))
                }
                // Switched on but not connected right now.
                (enabled - seen).sorted().forEach { iface ->
                    val port = portOf(iface)
                    say(listOf("net", iface, "-", "", "0", "0", "1", port.toString(), "disconnected").joinToString("\t"))
                }
                say("router\t$ROUTER_PORT\t${proxyState(enabled.isNotEmpty(), ROUTER_PORT)}")
                rules().forEach { say("rule\t${it.cidr}\t${it.iface}") }
                say("")
            }
            "enable", "disable" -> {
                val iface = args.getOrNull(1) ?: return say("err usage: ${args[0]} <interface>")
                val enabled = enabledIfaces().toMutableSet()
                if (args[0] == "enable") enabled += iface else enabled -= iface
                prefs().edit().putStringSet("enabled", enabled).apply()
                if (args[0] == "enable") portOf(iface)
                applyProxies()
                say("ok")
            }
            "rule-add" -> {
                val rule = args.getOrNull(2)?.let { parseRule(args[1], it) }
                    ?: return say("err usage: rule-add <a.b.c.d/prefix> <interface>")
                saveRules(rules().filter { it.cidr != rule.cidr } + rule)
                say("ok")
            }
            "rule-del" -> {
                val cidr = args.getOrNull(1)?.let { parseRule(it, "-")?.cidr }
                    ?: return say("err usage: rule-del <a.b.c.d/prefix>")
                val rules = rules()
                if (rules.none { it.cidr == cidr }) return say("err no rule for $cidr")
                saveRules(rules.filter { it.cidr != cidr })
                say("ok")
            }
            "ping" -> {
                val iface = args.getOrNull(1)
                val host = args.getOrNull(2)
                if (iface == null || host == null) return say("err usage: ping <interface|auto> <host> [count]")
                ping(iface.takeIf { it != "auto" }, host, args.getOrNull(3)?.toIntOrNull() ?: 4, ::say)
                say("done")
            }
            "forward" -> forward(args, client, ::say)
            else -> say("err unknown request")
        }
    }

    private fun proxyState(on: Boolean, port: Int): String = when {
        !on -> "off"
        proxies[port]?.isClosed == false -> "ok"
        else -> "busy"
    }

    // ── ping through a network (ICMP datagram socket, allowed for apps) ──

    private fun ping(iface: String?, host: String, count: Int, say: (String) -> Unit) {
        val address = try {
            resolve(iface, host)
        } catch (error: Exception) {
            return say("err cannot resolve $host: ${error.message}")
        }
        if (address !is Inet4Address) return say("err IPv4 only")
        val route = routeFor(iface, address)
        if (route is Route.Unreachable) return say("err ${route.reason}")
        val fd = Os.socket(OsConstants.AF_INET, OsConstants.SOCK_DGRAM, OsConstants.IPPROTO_ICMP)
        try {
            if (route is Route.Via) route.network.bindSocket(fd)
            val via = (route as? Route.Via)?.let { r -> networks[r.network]?.iface } ?: "default"
            say("PING ${address.hostAddress} via $via")
            var received = 0
            for (sequence in 1..count.coerceIn(1, 100)) {
                val packet = ByteArray(24)
                packet[0] = 8
                packet[6] = (sequence shr 8).toByte()
                packet[7] = sequence.toByte()
                val sent = System.nanoTime()
                Os.sendto(fd, packet, 0, packet.size, 0, address, 0)
                val deadline = sent + 1_000_000_000L
                var answered = false
                while (!answered) {
                    val left = ((deadline - System.nanoTime()) / 1_000_000).toInt()
                    if (left <= 0) break
                    val poll = StructPollfd().apply { this.fd = fd; events = OsConstants.POLLIN.toShort() }
                    if (Os.poll(arrayOf(poll), left) <= 0) break
                    val buffer = ByteArray(1500)
                    val read = Os.recvfrom(fd, buffer, 0, buffer.size, 0, null)
                    val replySequence = ((buffer[6].toInt() and 0xFF) shl 8) or (buffer[7].toInt() and 0xFF)
                    if (read >= 8 && buffer[0].toInt() == 0 && replySequence == sequence) {
                        val ms = (System.nanoTime() - sent) / 1e6
                        say("reply from ${address.hostAddress}: seq=$sequence time=${"%.1f".format(ms)} ms")
                        received++
                        answered = true
                    }
                }
                if (!answered) say("timeout seq=$sequence")
                val pause = (deadline - System.nanoTime()) / 1_000_000
                if (sequence < count && pause > 0) Thread.sleep(pause)
            }
            say("$received/$count replies")
        } catch (error: Exception) {
            say("err ${error.message}")
        } finally {
            runCatching { Os.close(fd) }
        }
    }

    // ── Port forwarding (UDP, or TCP for programs without SOCKS) ──

    private fun forward(args: List<String>, client: LocalSocket, say: (String) -> Unit) {
        val protocol = args.getOrNull(1)
        val localPort = args.getOrNull(2)?.toIntOrNull()
        val iface = args.getOrNull(3)?.takeIf { it != "auto" }
        val host = args.getOrNull(4)
        val port = args.getOrNull(5)?.toIntOrNull()
        if (protocol !in setOf("tcp", "udp") || localPort == null || args.getOrNull(3) == null || host == null || port == null) {
            return say("err usage: forward <tcp|udp> <local port> <interface|auto> <host> <port>")
        }
        val address = try {
            resolve(iface, host)
        } catch (error: Exception) {
            return say("err cannot resolve $host: ${error.message}")
        }
        val route = routeFor(iface, address)
        if (route is Route.Unreachable) return say("err ${route.reason}")
        val loopback = InetAddress.getByName("127.0.0.1")
        val closers = mutableListOf<java.io.Closeable>()
        try {
            if (protocol == "tcp") {
                val listener = ServerSocket(localPort, 50, loopback)
                closers += listener
                thread(name = "net-forward-tcp", isDaemon = true) {
                    acceptLoop(listener) { local -> connect(route, address, port).use { pipe(local, it) } }
                }
                say("ok ${listener.localPort}")
            } else {
                val local = DatagramSocket(InetSocketAddress(loopback, localPort))
                val remote = DatagramSocket()
                closers += local
                closers += remote
                if (route is Route.Via) route.network.bindSocket(remote)
                remote.connect(address, port)
                val peer = java.util.concurrent.atomic.AtomicReference<InetSocketAddress?>(null)
                thread(name = "net-forward-udp-out", isDaemon = true) {
                    val buffer = ByteArray(65_535)
                    runCatching {
                        while (true) {
                            val packet = DatagramPacket(buffer, buffer.size)
                            local.receive(packet)
                            peer.set(packet.socketAddress as InetSocketAddress)
                            remote.send(DatagramPacket(packet.data, packet.offset, packet.length))
                        }
                    }
                }
                thread(name = "net-forward-udp-in", isDaemon = true) {
                    val buffer = ByteArray(65_535)
                    runCatching {
                        while (true) {
                            val packet = DatagramPacket(buffer, buffer.size)
                            remote.receive(packet)
                            peer.get()?.let { local.send(DatagramPacket(packet.data, packet.offset, packet.length, it)) }
                        }
                    }
                }
                say("ok ${local.localPort}")
            }
            // Forwards until the client goes away.
            while (client.inputStream.read() >= 0) Unit
        } catch (error: Exception) {
            runCatching { say("err ${error.message}") }
        } finally {
            closers.forEach { runCatching { it.close() } }
        }
    }
}

package com.example.piliplus.vr;

import android.util.Base64;
import android.util.Log;

import org.json.JSONArray;
import org.json.JSONObject;

import java.io.ByteArrayOutputStream;
import java.io.DataInputStream;
import java.io.EOFException;
import java.io.IOException;
import java.io.InputStream;
import java.io.OutputStream;
import java.net.HttpURLConnection;
import java.net.SocketTimeoutException;
import java.net.URI;
import java.net.URL;
import java.net.URLEncoder;
import java.nio.ByteBuffer;
import java.nio.ByteOrder;
import java.nio.charset.StandardCharsets;
import java.security.MessageDigest;
import java.security.SecureRandom;
import java.util.ArrayList;
import java.util.HashMap;
import java.util.List;
import java.util.Map;
import java.util.TreeMap;
import java.util.zip.DataFormatException;
import java.util.zip.Inflater;

import javax.net.ssl.SSLSocket;
import javax.net.ssl.SSLSocketFactory;

/**
 * Standalone BiliLive danmaku feed for immersive VR.
 *
 * The 2D live room writes DANMU_MSG into vr_live_danmaku.tsv, but its Flutter
 * WebSocket is owned by the 2D engine. Once that engine is detached while VR
 * takes over, the file stops growing and the VR danmaku layer has no source.
 * This client keeps the feed on the native VR side: Wbi-signed getDanmuInfo,
 * WSS handshake, protover=2 auth, heartbeat and DANMU_MSG decoding.
 */
public final class LiveDanmakuClient {
    private static final String TAG = "PiliVR-LiveDM";
    private static final int[] MIXIN_KEY_TAB = {
            46, 47, 18, 2, 53, 8, 23, 32, 15, 50, 10, 31, 58, 3, 45, 35,
            27, 43, 5, 49, 33, 9, 42, 19, 29, 28, 14, 39, 12, 38, 41, 13
    };

    public interface Sink {
        void onDanmaku(int mode, int color, String text);
        void onLog(String message);
        void onClosed();
    }

    private static final class Config {
        final String token;
        final List<String> servers;
        Config(String token, List<String> servers) {
            this.token = token;
            this.servers = servers;
        }
    }

    private static final class Frame {
        final int opcode;
        final boolean fin;
        final byte[] payload;
        Frame(int opcode, boolean fin, byte[] payload) {
            this.opcode = opcode;
            this.fin = fin;
            this.payload = payload;
        }
    }

    private final long roomId;
    private final Sink sink;
    private final SecureRandom random = new SecureRandom();
    private volatile boolean running;
    private volatile SSLSocket socket;
    private Thread thread;
    private String cookie = "";
    private int statFrames;
    private int statPackets;
    private int statV0;
    private int statV1;
    private int statV2;
    private int statV3;
    private int statDanmakuCmds;
    private int statEmitted;
    private long lastStatsMs;
    private final HashMap<String, Integer> statCmds = new HashMap<>();

    public LiveDanmakuClient(long roomId, Sink sink) {
        this.roomId = roomId;
        this.sink = sink;
    }

    public long getRoomId() {
        return roomId;
    }

    public synchronized void start() {
        if (thread != null && thread.isAlive()) return;
        running = true;
        thread = new Thread(new Runnable() {
            @Override
            public void run() {
                try {
                    runClient();
                } catch (Throwable t) {
                    log("live feed failed: " + t);
                } finally {
                    running = false;
                    try {
                        sink.onClosed();
                    } catch (Throwable ignored) {
                    }
                }
            }
        }, "PiliVRLiveDanmaku");
        thread.setDaemon(true);
        thread.start();
    }

    public synchronized void stop() {
        running = false;
        SSLSocket s = socket;
        if (s != null) {
            try {
                s.close();
            } catch (Throwable ignored) {
            }
        }
        Thread t = thread;
        if (t != null) t.interrupt();
    }

    private void log(String message) {
        Log.i(TAG, message);
        try {
            sink.onLog(message);
        } catch (Throwable ignored) {
        }
    }

    private void logStats() {
        log("stats frames=" + statFrames + " packets=" + statPackets
                + " v0=" + statV0 + " v1=" + statV1 + " v2=" + statV2
                + " v3=" + statV3 + " dmCmds=" + statDanmakuCmds
                + " emitted=" + statEmitted + " cmds=" + statCmds);
        statCmds.clear();
    }

    private void runClient() throws Exception {
        Config config = fetchConfig(roomId);
        if (config.token.isEmpty() || config.servers.isEmpty()) {
            throw new IllegalStateException("empty live danmaku config");
        }
        log("config ready room=" + roomId + " servers=" + config.servers.size());
        Exception last = null;
        for (String server : config.servers) {
            if (!running) return;
            try {
                connect(server, config.token);
                return;
            } catch (Exception e) {
                last = e;
                log("server failed " + server + ": " + e);
            }
        }
        if (last != null) throw last;
    }

    private Config fetchConfig(long room) throws Exception {
        String mixinKey = fetchMixinKey();
        cookie = fetchCookie();
        TreeMap<String, String> params = new TreeMap<>();
        params.put("id", Long.toString(room));
        params.put("web_location", "444.8");
        params.put("wts", Long.toString(System.currentTimeMillis() / 1000L));
        StringBuilder query = new StringBuilder();
        for (Map.Entry<String, String> e : params.entrySet()) {
            if (query.length() > 0) query.append('&');
            query.append(URLEncoder.encode(e.getKey(), "UTF-8")).append('=')
                    .append(URLEncoder.encode(filterValue(e.getValue()), "UTF-8"));
        }
        String url = "https://api.live.bilibili.com/xlive/web-room/v1/index/getDanmuInfo?"
                + query + "&w_rid=" + md5(query.toString() + mixinKey);
        JSONObject root = new JSONObject(httpGet(url, cookie));
        if (root.optInt("code", -1) != 0) {
            throw new IllegalStateException("getDanmuInfo code=" + root.optInt("code", -1));
        }
        JSONObject data = root.getJSONObject("data");
        String token = data.optString("token", "");
        List<String> servers = new ArrayList<>();
        JSONArray hosts = data.optJSONArray("host_list");
        if (hosts != null) {
            for (int i = 0; i < hosts.length() && servers.size() < 4; i++) {
                JSONObject h = hosts.optJSONObject(i);
                if (h == null) continue;
                String host = h.optString("host", "");
                int port = h.optInt("wss_port", 443);
                if (host.isEmpty() || port <= 0) continue;
                String server = "wss://" + host + ":" + port + "/sub";
                if (!servers.contains(server)) servers.add(server);
            }
        }
        return new Config(token, servers);
    }

    private String fetchMixinKey() throws Exception {
        JSONObject root = new JSONObject(httpGet("https://api.bilibili.com/x/web-interface/nav"));
        JSONObject data = root.optJSONObject("data");
        if (data == null) throw new IllegalStateException("nav has no data");
        JSONObject wbi = data.optJSONObject("wbi_img");
        if (wbi == null) throw new IllegalStateException("nav has no wbi_img");
        String orig = fileName(wbi.optString("img_url", ""))
                + fileName(wbi.optString("sub_url", ""));
        if (orig.length() < 64) throw new IllegalStateException("invalid wbi keys");
        char[] mixed = new char[32];
        for (int i = 0; i < MIXIN_KEY_TAB.length; i++) {
            mixed[i] = orig.charAt(MIXIN_KEY_TAB[i]);
        }
        return new String(mixed);
    }

    private static String fetchCookie() {
        try {
            JSONObject root = new JSONObject(httpGet(
                    "https://api.bilibili.com/x/frontend/finger/spi", null));
            JSONObject data = root.optJSONObject("data");
            if (data == null) return "";
            String b3 = data.optString("b_3", "");
            String b4 = data.optString("b_4", "");
            if (b3.isEmpty()) return "";
            return "buvid3=" + b3 + (b4.isEmpty() ? "" : "; buvid4=" + b4);
        } catch (Throwable ignored) {
            return "";
        }
    }

    private static String filterValue(String value) {
        StringBuilder out = new StringBuilder();
        for (int i = 0; i < value.length(); i++) {
            char c = value.charAt(i);
            if (c != '!' && c != '\'' && c != '(' && c != ')' && c != '*') out.append(c);
        }
        return out.toString();
    }

    private static String fileName(String url) {
        int slash = url.lastIndexOf('/');
        String name = slash >= 0 ? url.substring(slash + 1) : url;
        int dot = name.lastIndexOf('.');
        return dot > 0 ? name.substring(0, dot) : name;
    }

    private static String md5(String text) throws Exception {
        MessageDigest digest = MessageDigest.getInstance("MD5");
        byte[] bytes = digest.digest(text.getBytes(StandardCharsets.UTF_8));
        StringBuilder out = new StringBuilder(bytes.length * 2);
        for (byte b : bytes) {
            out.append(Character.forDigit((b >> 4) & 0xF, 16));
            out.append(Character.forDigit(b & 0xF, 16));
        }
        return out.toString();
    }

    private static String httpGet(String urlText) throws Exception {
        return httpGet(urlText, null);
    }

    private static String httpGet(String urlText, String cookie) throws Exception {
        HttpURLConnection conn = null;
        InputStream in = null;
        try {
            conn = (HttpURLConnection) new URL(urlText).openConnection();
            conn.setRequestMethod("GET");
            conn.setConnectTimeout(6000);
            conn.setReadTimeout(6000);
            conn.setRequestProperty("User-Agent", "Mozilla/5.0 (Linux; Android 12; Quest 3)");
            conn.setRequestProperty("Referer", "https://live.bilibili.com/");
            conn.setRequestProperty("Origin", "https://live.bilibili.com");
            conn.setRequestProperty("Accept", "application/json, text/plain, */*");
            if (cookie != null && !cookie.isEmpty()) {
                conn.setRequestProperty("Cookie", cookie);
            }
            int code = conn.getResponseCode();
            if (code < 200 || code >= 300) throw new IOException("HTTP " + code);
            in = conn.getInputStream();
            ByteArrayOutputStream out = new ByteArrayOutputStream();
            byte[] buf = new byte[4096];
            int n;
            while ((n = in.read(buf)) > 0) out.write(buf, 0, n);
            return new String(out.toByteArray(), StandardCharsets.UTF_8);
        } finally {
            if (in != null) try { in.close(); } catch (Throwable ignored) {}
            if (conn != null) conn.disconnect();
        }
    }

    private void connect(String server, String token) throws Exception {
        URI uri = URI.create(server);
        String host = uri.getHost();
        int port = uri.getPort() > 0 ? uri.getPort() : 443;
        String path = uri.getRawPath();
        if (host == null || path == null || path.isEmpty()) {
            throw new IllegalArgumentException("bad server " + server);
        }
        SSLSocket s = (SSLSocket) SSLSocketFactory.getDefault().createSocket(host, port);
        socket = s;
        s.setSoTimeout(8000);
        s.startHandshake();
        OutputStream out = s.getOutputStream();
        InputStream in = s.getInputStream();
        byte[] nonce = new byte[16];
        random.nextBytes(nonce);
        String key = Base64.encodeToString(nonce, Base64.NO_WRAP);
        String cookieHeader = (cookie == null || cookie.isEmpty())
                ? "" : "Cookie: " + cookie + "\r\n";
        String request = "GET " + path + " HTTP/1.1\r\n"
                + "Host: " + host + ":" + port + "\r\n"
                + "Upgrade: websocket\r\n"
                + "Connection: Upgrade\r\n"
                + "Sec-WebSocket-Key: " + key + "\r\n"
                + "Sec-WebSocket-Version: 13\r\n"
                + "Origin: https://live.bilibili.com\r\n"
                + cookieHeader
                + "User-Agent: Mozilla/5.0 (Linux; Android 12; Quest 3)\r\n\r\n";
        out.write(request.getBytes(StandardCharsets.ISO_8859_1));
        out.flush();
        readHttpHeaders(in);
        s.setSoTimeout(30000);

        JSONObject auth = new JSONObject();
        auth.put("uid", 0);
        auth.put("roomid", roomId);
        auth.put("protover", 2); // zlib, avoiding brotli which Android lacks
        auth.put("platform", "web");
        auth.put("type", 2);
        auth.put("key", token);
        byte[] authBody = auth.toString().getBytes(StandardCharsets.UTF_8);
        writeFrame(out, 2, makePacket(1, 7, 1, authBody));
        log("connected " + host + ":" + port + ", auth sent");

        DataInputStream dataIn = new DataInputStream(in);
        ByteArrayOutputStream fragment = new ByteArrayOutputStream();
        int fragmentOpcode = 0;
        long lastHeartbeatMs = System.currentTimeMillis();
        lastStatsMs = lastHeartbeatMs;
        int heartbeatSeq = 1;
        while (running) {
            try {
                Frame frame = readFrame(dataIn);
                if (frame == null) break;
                statFrames++;
                if (frame.opcode == 9) {
                    writeFrame(out, 10, frame.payload);
                    continue;
                }
                if (frame.opcode == 8) break;
                if (frame.opcode == 2 && frame.fin) {
                    processBili(frame.payload);
                    fragment.reset();
                    fragmentOpcode = 0;
                } else if (frame.opcode == 2) {
                    fragmentOpcode = 2;
                    fragment.reset();
                    fragment.write(frame.payload);
                } else if (frame.opcode == 0 && fragmentOpcode == 2) {
                    fragment.write(frame.payload);
                    if (frame.fin) {
                        processBili(fragment.toByteArray());
                        fragment.reset();
                        fragmentOpcode = 0;
                    }
                }
            } catch (SocketTimeoutException e) {
                // No message for 30 s: keep the long connection alive.
                if (!running) break;
            }
            long now = System.currentTimeMillis();
            if (now - lastHeartbeatMs >= 30000L) {
                writeFrame(out, 2, makePacket(1, 2, heartbeatSeq++, null));
                lastHeartbeatMs = now;
            }
            if (now - lastStatsMs >= 10000L) {
                logStats();
                lastStatsMs = now;
            }
        }
    }

    private static void readHttpHeaders(InputStream in) throws IOException {
        ByteArrayOutputStream headers = new ByteArrayOutputStream();
        byte[] pattern = {'\r', '\n', '\r', '\n'};
        int matched = 0;
        while (matched < pattern.length) {
            int b = in.read();
            if (b < 0) throw new EOFException("WebSocket handshake ended early");
            headers.write(b);
            if (b == pattern[matched]) {
                matched++;
            } else {
                matched = (b == '\r') ? 1 : 0;
            }
        }
        String response = new String(headers.toByteArray(), StandardCharsets.ISO_8859_1);
        if (!response.contains(" 101 ")) {
            throw new IOException("WebSocket upgrade failed: " + response.split("\r\n")[0]);
        }
    }

    private static byte[] makePacket(int protocolVersion, int operation, int sequence,
            byte[] body) {
        int bodyLen = body == null ? 0 : body.length;
        ByteBuffer buffer = ByteBuffer.allocate(16 + bodyLen).order(ByteOrder.BIG_ENDIAN);
        buffer.putInt(16 + bodyLen);
        buffer.putShort((short) 16);
        buffer.putShort((short) protocolVersion);
        buffer.putInt(operation);
        buffer.putInt(sequence);
        if (bodyLen > 0) buffer.put(body);
        return buffer.array();
    }

    private void writeFrame(OutputStream out, int opcode, byte[] payload) throws IOException {
        int len = payload.length;
        synchronized (out) {
            out.write(0x80 | opcode);
            if (len < 126) {
                out.write(0x80 | len);
            } else if (len <= 0xFFFF) {
                out.write(0x80 | 126);
                out.write((len >> 8) & 0xFF);
                out.write(len & 0xFF);
            } else {
                out.write(0x80 | 127);
                for (int shift = 56; shift >= 0; shift -= 8) {
                    out.write((int) (((long) len >> shift) & 0xFF));
                }
            }
            byte[] mask = new byte[4];
            random.nextBytes(mask);
            out.write(mask);
            for (int i = 0; i < len; i++) {
                out.write(payload[i] ^ mask[i & 3]);
            }
            out.flush();
        }
    }

    private static Frame readFrame(DataInputStream in) throws IOException {
        int first;
        try {
            first = in.readUnsignedByte();
        } catch (EOFException e) {
            return null;
        }
        int second = in.readUnsignedByte();
        boolean fin = (first & 0x80) != 0;
        int opcode = first & 0x0F;
        long len = second & 0x7F;
        if (len == 126) {
            len = in.readUnsignedShort();
        } else if (len == 127) {
            len = in.readLong();
            if (len < 0 || len > 8 * 1024 * 1024) throw new IOException("frame too large: " + len);
        }
        byte[] mask = null;
        if ((second & 0x80) != 0) {
            mask = new byte[4];
            in.readFully(mask);
        }
        byte[] payload = new byte[(int) len];
        in.readFully(payload);
        if (mask != null) {
            for (int i = 0; i < payload.length; i++) payload[i] ^= mask[i & 3];
        }
        return new Frame(opcode, fin, payload);
    }

    private void processBili(byte[] packet) {
        int offset = 0;
        while (offset + 16 <= packet.length) {
            int total = readU32(packet, offset);
            int headerLen = readU16(packet, offset + 4);
            int version = readU16(packet, offset + 6);
            int operation = readU32(packet, offset + 8);
            if (total < headerLen || headerLen < 16 || offset + total > packet.length) return;
            statPackets++;
            if (version == 0) statV0++;
            else if (version == 1) statV1++;
            else if (version == 2) statV2++;
            else if (version == 3) statV3++;
            if (version == 2) {
                byte[] inflated = inflate(packet, offset + headerLen, total - headerLen);
                if (inflated != null) processBili(inflated);
            } else if (version == 0 || version == 1) {
                if (operation == 5) {
                    parseCommand(packet, offset + headerLen, total - headerLen);
                }
            }
            offset += total;
        }
    }

    private static byte[] inflate(byte[] src, int offset, int length) {
        Inflater inflater = new Inflater();
        try {
            inflater.setInput(src, offset, length);
            ByteArrayOutputStream out = new ByteArrayOutputStream(length * 3);
            byte[] buf = new byte[4096];
            while (!inflater.finished()) {
                int n = inflater.inflate(buf);
                if (n > 0) out.write(buf, 0, n);
                else if (inflater.needsDictionary() || inflater.needsInput()) break;
            }
            return out.toByteArray();
        } catch (DataFormatException e) {
            return null;
        } finally {
            inflater.end();
        }
    }

    private void parseCommand(byte[] src, int offset, int length) {
        if (length <= 0) return;
        try {
            String text = new String(src, offset, length, StandardCharsets.UTF_8);
            JSONObject obj = new JSONObject(text);
            String cmd = obj.optString("cmd");
            String key = cmd == null || cmd.isEmpty() ? "?" : cmd;
            Integer count = statCmds.get(key);
            statCmds.put(key, count == null ? 1 : count + 1);
            // B站会给新协议弹幕附加版本后缀，例如
            // DANMU_MSG:4:0:2:2:2:0；精确匹配会漏掉这类弹幕。
            if (key.startsWith("DANMU_MSG")) {
                statDanmakuCmds++;
            } else if ("SUPER_CHAT_MESSAGE".equals(key)
                    || "SUPER_CHAT_MESSAGE_JPN".equals(key)) {
                JSONObject data = obj.optJSONObject("data");
                String sc = data == null ? "" : sanitize(data.optString("message", ""));
                if (!sc.isEmpty()) {
                    statEmitted++;
                    sink.onDanmaku(1, 0xFFFFFF, sc);
                }
                return;
            } else {
                return;
            }
            JSONArray info = obj.optJSONArray("info");
            if (info == null || info.length() < 2) return;
            Object messageObj = info.opt(1);
            if (messageObj == null) return;
            String message = sanitize(messageObj.toString());
            if (message.isEmpty()) return;
            int mode = 1;
            int color = 0xFFFFFF;
            JSONArray first = info.optJSONArray(0);
            if (first != null) {
                // Older/newer packets may not carry info[0][15].extra. The
                // original DANMU_MSG array already has mode and color here.
                int fallbackMode = first.optInt(1, mode);
                if (fallbackMode >= 1 && fallbackMode <= 5) mode = fallbackMode;
                int fallbackColor = first.optInt(3, color);
                if (fallbackColor > 0) color = fallbackColor;
            }
            if (first != null && first.length() > 15) {
                Object contentObj = first.opt(15);
                JSONObject content = null;
                if (contentObj instanceof JSONObject) {
                    content = (JSONObject) contentObj;
                } else if (contentObj != null) {
                    try {
                        content = new JSONObject(contentObj.toString());
                    } catch (Throwable ignored) {
                    }
                }
                if (content != null) {
                    Object extraObj = content.opt("extra");
                    JSONObject extra = null;
                    if (extraObj instanceof JSONObject) {
                        extra = (JSONObject) extraObj;
                    } else if (extraObj != null) {
                        try {
                            extra = new JSONObject(extraObj.toString());
                        } catch (Throwable ignored) {
                        }
                    }
                    if (extra != null) {
                        mode = extra.optInt("mode", mode);
                        int extraColor = extra.optInt("color", color);
                        if (extraColor > 0) color = extraColor;
                    }
                }
            }
            if (mode < 1 || mode > 5) mode = 1;
            statEmitted++;
            sink.onDanmaku(mode, color, message);
        } catch (Throwable ignored) {
        }
    }

    private static String sanitize(String text) {
        return text.replace('\r', ' ').replace('\n', ' ').replace('\t', ' ').trim();
    }

    private static int readU16(byte[] data, int offset) {
        return ((data[offset] & 0xFF) << 8) | (data[offset + 1] & 0xFF);
    }

    private static int readU32(byte[] data, int offset) {
        return ((data[offset] & 0xFF) << 24) | ((data[offset + 1] & 0xFF) << 16)
                | ((data[offset + 2] & 0xFF) << 8) | (data[offset + 3] & 0xFF);
    }
}

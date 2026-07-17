// SPDX-License-Identifier: MIT
//
// LiteCode-CPP — Docker client (Phase 4 ★)
//
// SPEC §11 Phase 4 / §7.3 / §15.5
//   Thread-safe, project-shaped wrapper around the Docker Engine HTTP
//   API that the web process uses to drive the `litecode-judge` image
//   through the [tecnativa/docker-socket-proxy](https://github.com/Tecnativa/docker-socket-proxy).
//   We deliberately talk HTTP, not the raw unix socket, because SPEC
//   §3.1 / §15.5 insists on routing every Docker call through the
//   proxy:
//
//     "Web → Docker 经 socket-proxy（白名单 5 子命令）"
//
//   Whitelisted endpoints (compose file environment):
//     POST   /containers/create           CONTAINERS=1
//     POST   /containers/{id}/start       CONTAINERS=1
//     POST   /containers/{id}/exec        EXEC=1
//     POST   /containers/{id}/kill        CONTAINERS=1
//     DELETE /containers/{id}             CONTAINERS=1 (DELETE method allowed)
//     GET    /_ping                       PING=1   — health probe
//     GET    /info                        INFO=1   — version introspection
//
//   Everything else (image pull, network mgmt, exec start, attach, ...)
//   is intentionally NOT exposed by the proxy and therefore NOT in
//   this client. The judge container runs as the image's ENTRYPOINT,
//   so we never need to attach or stream logs through the API — we
//   read the JSON result off the container's stdout via `docker logs`
//   (a documented `GET /containers/{id}/logs` we treat as auxiliary).
//
// Header-only + inline so the rest of Phase 4 (judge_scheduler.h,
// warm_pool.h) can compose against it without a separate translation
// unit, matching system_routes.h / connection_pool.h.
//
// Threading model:
//   The client itself is stateless. Every API call constructs a fresh
//   httplib::Client — that's the cheapest reliable way to side-step
//   httplib's "a single Client isn't safe to share across threads
//   simultaneously" caveat while keeping the API surface simple. Per-
//   call client construction is negligible against the g++ compile
//   cost of a judge.
//
// Failure surface:
//   `throw` is the only way callers learn a problem. Five named
//   exception types so callers (warm_pool, judge_scheduler,
//   system_routes::make_docker_probe) can recover vs. log differently:
//
//     DockerClientError     base — anything we couldn't talk to docker
//     DockerConfigError     Client construction / URL parse failure
//     DockerHttpError       daemon returned non-2xx (carries status)
//     DockerTimeoutError    connection / read / wait timed out
//
//   `ping()` is the one entry point that DOES NOT throw — it has to
//   keep /api/v1/health from crashing (SPEC A31).
//
// Usage:
//   auto client = litecode::docker::Client("tcp://docker-proxy:2375");
//   if (!client.ping()) { LOG_WARN("docker proxy unreachable"); }
//
//   litecode::docker::CreateOptions opts;
//   opts.image      = cfg.judge.judge_image;   // litecode-judge:latest
//   opts.command    = {"--help"};              // keep idle for warm pool
//   opts.memory_mb  = 64;
//   opts.cpus       = 0.0;                     // no CPU cap on idle
//   opts.env        = {"JUDGE_HOME=/judge"};
//   auto c = client.create(opts);
//   client.start(c.id);
//   // ...later...
//   auto w = client.wait(c.id, /*timeout_ms=*/30'000);
//   client.remove(c.id);
//
#pragma once

#include <cerrno>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <map>
#include <memory>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include <httplib.h>
#include <nlohmann/json.hpp>

#include "../config.h"                  // JudgeConfig
#include "../logger.h"                  // LOG_WARN
#include "../routes/system_routes.h"    // HealthService::Probe / ProbeResult

#ifdef _WIN32
#  include <windows.h>                  // ::GetLastError
#endif

namespace litecode {
namespace docker {

// ────────────────────────────────────────────────────────────────────────────
//  Exceptions
// ────────────────────────────────────────────────────────────────────────────

class DockerClientError : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

// Misconfigured client (URL parse failure, negative timeout, ...).
// Almost always a bug — caught at boot, not at runtime.
class DockerConfigError : public DockerClientError {
public:
    using DockerClientError::DockerClientError;
};

// Docker (proxy) returned a non-2xx, or the body wasn't the JSON we asked
// for. Carries the HTTP status so callers can distinguish 404 (gone,
// warm-pool already pruned it) from 5xx (daemon overloaded).
class DockerHttpError : public DockerClientError {
public:
    DockerHttpError(int status, std::string url, std::string body)
        : DockerClientError(build_message(status, url, body)),
          status_(status),
          url_(std::move(url)),
          body_(std::move(body)) {}

    int                status() const noexcept { return status_; }
    const std::string& url()    const noexcept { return url_; }
    const std::string& body()   const noexcept { return body_; }

private:
    static std::string build_message(int status, const std::string& url,
                                     const std::string& body) {
        std::ostringstream os;
        os << "docker http " << status << " on " << url
           << " (body=" << body.size() << " bytes): " << body;
        return os.str();
    }

    int         status_;
    std::string url_;
    std::string body_;
};

// Network-level timeout (connect / read / wait). Distinct from
// DockerHttpError so callers can retry vs. abort.
class DockerTimeoutError : public DockerClientError {
public:
    using DockerClientError::DockerClientError;
};

// ────────────────────────────────────────────────────────────────────────────
//  Endpoint / URL
//
//  We accept only the trivial `scheme://host:port[/base]` shape that
//  the docker socket proxy uses (tcp/http only — no TLS, no unix socket,
//  no @-credentials, no query/fragment). Anything else is a
//  configuration error and we throw DockerConfigError so the boot path
//  can surface it cleanly.
// ────────────────────────────────────────────────────────────────────────────

struct Endpoint {
    std::string   scheme   = "http";        // only http / tcp accepted
    std::string   host;
    std::uint16_t port     = 2375;
    std::string   base     = "/";           // path prefix; default "/"
    std::int32_t  timeout_ms = 30'000;      // per-call default

    // Render as `http://host:port` form that httplib::Client
    // (scheme_host_port ctor) and curl accept verbatim. cpp-httplib
    // v0.18.3 rejects `tcp://` outright with "'tcp' scheme is not
    // supported" (httplib.h:9674) — but our proxy speaks plain
    // HTTP-over-TCP, so we treat the user-facing `tcp://` as a
    // synonym for `http://`. The `scheme` field is kept as-typed
    // for documentation / future TLS use; only the rendered URL
    // is normalized.
    std::string to_url() const {
        std::ostringstream os;
        os << (scheme == "tcp" ? "http" : scheme)
           << "://" << host << ":" << port;
        return os.str();
    }

    // Concatenate base + path without double-slash trouble. The proxy
    // lives at the daemon root, so `base == "/"` is normal; we keep
    // the seam in case someone front-ends it with a /v1.40 prefix.
    std::string join(const std::string& path) const {
        std::string out = base;
        if (out.empty() || out.back() != '/') out.push_back('/');
        if (!path.empty() && path.front() == '/') {
            out += path.substr(1);
        } else {
            out += path;
        }
        return out;
    }
};

// ────────────────────────────────────────────────────────────────────────────
//  Forward declarations
//
//  detail::build_create_body takes a const ref to CreateOptions, which
//  is defined further down. C++ allows incomplete-type-by-reference at
//  declaration, but MSVC's two-phase lookup is happier with a forward
//  declaration in the same enclosing namespace.
// ────────────────────────────────────────────────────────────────────────────

struct CreateOptions;

namespace detail {

// Very small URL parse for `tcp://host:port` and `http://host:port[/base]`.
// We refuse `unix://`, `npipe://`, https (no TLS), or anything with
// `@` / query / fragment — those aren't valid for our proxy and we
// shouldn't silently accept them.
//
// Throws DockerConfigError on anything malformed.
inline Endpoint parse_endpoint_url(const std::string& url) {
    Endpoint ep;

    // Split scheme://rest
    auto sep = url.find("://");
    if (sep == std::string::npos || sep == 0) {
        throw DockerConfigError("docker endpoint url missing scheme: " + url);
    }
    ep.scheme = url.substr(0, sep);
    for (auto& c : ep.scheme)
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    if (ep.scheme != "http" && ep.scheme != "tcp") {
        throw DockerConfigError(
            "docker endpoint scheme must be http or tcp, got '" +
            ep.scheme + "' in " + url);
    }
    std::string rest = url.substr(sep + 3);

    // Reject obviously-broken constructs so a stale env var doesn't
    // route an HTTP request at garbage.
    if (rest.find('@') != std::string::npos)
        throw DockerConfigError("docker endpoint url has '@' (no credentials): " + url);
    if (rest.find('#') != std::string::npos)
        throw DockerConfigError("docker endpoint url has fragment: " + url);
    if (rest.find('?') != std::string::npos)
        throw DockerConfigError("docker endpoint url has query string: " + url);

    // Path = part after first '/'
    auto slash = rest.find('/');
    std::string authority;
    if (slash == std::string::npos) {
        authority = rest;
        ep.base    = "/";
    } else {
        authority = rest.substr(0, slash);
        ep.base    = rest.substr(slash);
        if (ep.base.empty()) ep.base = "/";
    }

    // host[:port]
    auto colon = authority.find(':');
    if (colon == std::string::npos) {
        ep.host = authority;
        ep.port = 2375;
    } else {
        ep.host = authority.substr(0, colon);
        std::string port_s = authority.substr(colon + 1);
        if (ep.host.empty())
            throw DockerConfigError("docker endpoint missing host: " + url);
        try {
            int p = std::stoi(port_s);
            if (p < 1 || p > 65535)
                throw std::out_of_range("port out of range");
            ep.port = static_cast<std::uint16_t>(p);
        } catch (...) {
            throw DockerConfigError(
                "docker endpoint port invalid: '" + port_s + "' in " + url);
        }
    }
    if (ep.host.empty())
        throw DockerConfigError("docker endpoint missing host: " + url);
    return ep;
}

// Construct the JSON body of POST /containers/create.
//
// Defined LATER in this file (after `CreateOptions` is complete) so
// MSVC's two-phase lookup can see the full struct definition when
// it instantiates the body. Forward-declared above for callers that
// want to take a function pointer / template arg.
//
// `NetMode == "none"` is the *default* SPEC §7.3 isolation; callers
// don't have to set it.
//
// `SecurityOpt` defaults to nothing — SPEC §15.4 lists
// `no-new-privileges:true` as mandatory, but we leave it empty so
// callers (judge_scheduler) opt in explicitly; the warm-pool idle
// container shouldn't carry the same default. SPEC §7.3 shows the
// full set on the example docker run invocation.
//
// The judge container does not need to expose ports, attach volumes,
// or mount a host directory (other than the task.json bind-mount),
// so the schema is intentionally small.
inline nlohmann::json build_create_body(const CreateOptions& opts);

// Render a query string from a list of (key, value). Empty values are
// dropped (matches "force=true&v=false" type semantics).
inline std::string build_query(
    const std::vector<std::pair<std::string, std::string>>& qs)
{
    std::string out;
    for (const auto& [k, v] : qs) {
        if (v.empty()) continue;
        if (!out.empty()) out.push_back('&');
        out.append(k);
        out.push_back('=');
        for (char c : v) {
            if (c == ' ' || c == '&' || c == '=' || c == '?' || c == '#') {
                char esc[4] = { '%', 0, 0, 0 };
                static const char hex[] = "0123456789ABCDEF";
                esc[1] = hex[(static_cast<unsigned char>(c) >> 4) & 0xF];
                esc[2] = hex[ static_cast<unsigned char>(c)       & 0xF];
                out.append(esc, 3);
            } else {
                out.push_back(c);
            }
        }
    }
    return out;
}

// Strip the 8-byte Docker log frame headers from a /containers/:id/logs
// response, keeping only the bytes that belong to the streams the
// caller asked for (stdout / stderr / both).
//
// Format (Docker Engine v1.40+, multiplexed):
//   For each frame:
//     byte 0    : STREAM_TYPE  — 0 stdin (unused), 1 stdout, 2 stderr
//     bytes 1-3 : 0 (reserved)
//     bytes 4-7 : SIZE (uint32 big-endian)  — payload length
//     bytes 8..8+SIZE : payload
// The total body can chain multiple frames; we walk until we can't
// read a full 8-byte header. Frames whose stream is excluded are
// dropped (their `SIZE` payload is consumed to keep the walker in
// sync with the actual byte offsets).
//
// If the body doesn't start with a plausible stream-type byte
// (< 3), or `SIZE` would run past the end of the buffer, we assume
// the response is NOT multiplexed (some daemon / client combos
// return raw bytes when only one stream is requested) and return
// the body verbatim.
//
// v1.2.50: added after discovering the docker-proxy forwards
// multiplexed bytes from the engine, breaking parse_judge_result_json
// (the 8-byte prefix isn't '{', so every judge was marked SE with
// "no parseable result JSON").
inline std::string strip_docker_log_frames(const std::string& body,
                                           bool want_stdout,
                                           bool want_stderr) {
    if (body.size() < 8) return body;
    unsigned char first = static_cast<unsigned char>(body[0]);
    if (first > 2) {
        // Not multiplexed — return verbatim.
        return body;
    }
    std::string out;
    out.reserve(body.size());
    std::size_t pos = 0;
    while (pos + 8 <= body.size()) {
        const unsigned char* p =
            reinterpret_cast<const unsigned char*>(body.data() + pos);
        unsigned char stream = p[0];
        std::uint32_t size   = (static_cast<std::uint32_t>(p[4]) << 24) |
                                (static_cast<std::uint32_t>(p[5]) << 16) |
                                (static_cast<std::uint32_t>(p[6]) <<  8) |
                                (static_cast<std::uint32_t>(p[7])      );
        if (pos + 8 + size > body.size()) {
            // Trailing partial frame — treat the rest as raw and
            // stop walking. Better to surface partial output than
            // to drop it.
            out.append(body, pos, body.size() - pos);
            break;
        }
        bool keep = (stream == 1 && want_stdout) ||
                    (stream == 2 && want_stderr) ||
                    (stream == 0);  // stdin never appears post-start
        if (keep) {
            out.append(body, pos + 8, size);
        }
        pos += 8 + size;
    }
    // If we walked the entire buffer cleanly, `out` is the cleaned
    // body. If we broke out early, `out` already has the trailing
    // partial. Either way we return it.
    return out;
}

} // namespace detail

// ────────────────────────────────────────────────────────────────────────────
//  Options / result types
// ────────────────────────────────────────────────────────────────────────────

// Bind mount (host path → container path). Works in dev / non-Docker-
// Desktop hosts where the host filesystem is shared with the docker
// daemon. NOT portable to Docker Desktop Windows / macOS — see
// v1.2.50 known-issues for why bind mounts break in those setups.
struct BindMount {
    std::string host_path;       // absolute host path
    std::string container_path;  // absolute container path
    bool        read_only = true;
};

// Named-volume mount. The volume name is a docker-managed identifier
// (e.g. "litecode-judge-tmp"). At container-create time the daemon
// resolves the name to its internal storage path and mounts the SAME
// storage to any container that references that name. This is the
// only mount type that survives Docker Desktop's host-VM boundary:
// the named volume lives inside the VM's docker-storage area, so the
// web process (writing via the named-volume mount at /tmp/litecode-
// judge) and the judge container (referencing Source="…judge-tmp")
// resolve to the same backing directory without any host-path
// translation.
//
// The first mount of an unknown volume name auto-creates it.
struct VolumeMount {
    std::string volume_name;     // docker volume name
    std::string container_path;  // absolute container path
    bool        read_only = false;
};

// Discriminated mount — used as the element type of
// CreateOptions::mounts. Build with docker::bind(...) /
// docker::volume(...); the rest of the call site reads
// mounts[i].kind() to route to the right Engine-API fields.
struct Mount {
    enum class Kind { Bind, Volume };
    Kind        kind = Kind::Bind;
    BindMount   bind;            // valid when kind == Bind
    VolumeMount volume;          // valid when kind == Volume

    static Mount bind_mount(std::string host_path,
                            std::string container_path,
                            bool read_only = true) {
        Mount m;
        m.kind = Kind::Bind;
        m.bind.host_path      = std::move(host_path);
        m.bind.container_path = std::move(container_path);
        m.bind.read_only      = read_only;
        return m;
    }
    static Mount volume_mount(std::string volume_name,
                              std::string container_path,
                              bool read_only = false) {
        Mount m;
        m.kind = Kind::Volume;
        m.volume.volume_name    = std::move(volume_name);
        m.volume.container_path = std::move(container_path);
        m.volume.read_only      = read_only;
        return m;
    }
    Kind kind_of() const noexcept { return kind; }
};

// Keep the bare BindMount constructor callable for the call sites
// that haven't migrated yet — builds a Mount::Bind under the hood.
inline Mount as_mount(BindMount b) {
    return Mount::bind_mount(std::move(b.host_path),
                             std::move(b.container_path),
                             b.read_only);
}
inline Mount as_mount(VolumeMount v) {
    return Mount::volume_mount(std::move(v.volume_name),
                               std::move(v.container_path),
                               v.read_only);
}

struct CreateOptions {
    std::string              image;       // required
    std::vector<std::string> command;     // CMD override; empty = image default
    std::vector<std::string> env;         // KEY=VALUE; passed to container
    std::string              working_dir; // optional
    std::string              user;        // optional, e.g. "judgeuser"

    // ── SPEC §7.3 / §15.4 isolation / resource caps ──
    std::string              network_mode;     // default "none"
    bool                     read_only     = true;
    std::int64_t             memory_mb     = 256;   // <= 0 → no cap
    double                   cpus          = 1.0;   // <= 0.0 → no cap
    std::int64_t             pids_limit    = 50;    // <= 0 → no cap

    // ── security / tmpfs / mounts ──
    std::vector<std::string> security_opt;
    std::map<std::string, std::string> tmpfs;
    std::vector<Mount>       mounts;
};

// Definition of `build_create_body` — HostConfig fields follow the
// Docker Engine API v1.40 schema verbatim so the proxy doesn't have
// to do anything clever. Lifted out of `detail` so that the full
// `CreateOptions` type is visible at the point of definition, which
// satisfies MSVC's two-phase lookup that a `namespace { struct X; }`
// forward declaration alone cannot.
namespace detail {
inline nlohmann::json build_create_body(const CreateOptions& opts) {
    nlohmann::json body;
    body["Image"]      = opts.image;
    body["AttachStdin"] = false;
    body["AttachStdout"] = false;
    body["AttachStderr"] = false;
    body["Tty"]         = false;
    body["OpenStdin"]   = false;
    body["StdinOnce"]   = false;
    if (!opts.command.empty())
        body["Cmd"] = opts.command;
    if (!opts.env.empty())
        body["Env"] = opts.env;
    if (!opts.working_dir.empty())
        body["WorkingDir"] = opts.working_dir;
    if (!opts.user.empty())
        body["User"] = opts.user;

    // HostConfig — resource caps + isolation flags (SPEC §7.3).
    nlohmann::json hc;
    if (opts.network_mode.empty())
        hc["NetworkMode"] = "none";
    else
        hc["NetworkMode"] = opts.network_mode;

    if (opts.read_only)
        hc["ReadonlyRootfs"] = true;

    if (opts.pids_limit > 0)
        hc["PidsLimit"] = opts.pids_limit;

    // Memory: bytes (negative or 0 → "no cap"). SPEC §7.3 defaults
    // to 256 MB; warm-pool idle containers should pass a smaller cap
    // (e.g. 64 MB) so a stuck idle container doesn't pin 256 MB.
    if (opts.memory_mb > 0)
        hc["Memory"] = static_cast<std::int64_t>(opts.memory_mb) * 1024 * 1024;

    // CPU: 1.0 cpus == 1e9 nano cpus. 0.0 means "no cap" — the API
    // accepts the field being absent.
    if (opts.cpus > 0.0)
        hc["NanoCpus"] = static_cast<std::int64_t>(opts.cpus * 1'000'000'000.0);

    if (!opts.security_opt.empty())
        hc["SecurityOpt"] = opts.security_opt;

    if (!opts.tmpfs.empty()) {
        nlohmann::json t;
        for (const auto& [k, v] : opts.tmpfs) t[k] = v;
        hc["Tmpfs"] = t;
    }

    // Mounts — Bind (host-path) and Volume (named) supported.
    // Tmpfs mounts come through `opts.tmpfs` instead, and the proxy
    // whitelist does not expose Volume-management endpoints — but the
    // /containers/create body can still declare volume mounts (they
    // auto-create the volume on first use, by Docker Engine spec).
    //
    // v1.2.50: switched the judge from a bind mount to a named
    // volume. Bind mounts work in dev / Linux hosts where the host
    // filesystem is the daemon's filesystem; on Docker Desktop
    // Windows / macOS, the host filesystem is *inside* the VM and the
    // proxy forwarding the bind source path resolves to a path that
    // the VM-side daemon can't see. A named volume lives in the
    // daemon's /var/lib/docker/volumes, so web (writing via the
    // named-volume mount at /tmp/litecode-judge) and the judge
    // container (mounting Source="…judge-tmp" at /tmp) share the
    // same backing directory regardless of where the host's /tmp
    // actually is.
    if (!opts.mounts.empty()) {
        nlohmann::json mounts = nlohmann::json::array();
        for (const auto& m : opts.mounts) {
            nlohmann::json j;
            if (m.kind_of() == Mount::Kind::Volume) {
                j["Type"]     = "volume";
                j["Source"]   = m.volume.volume_name;
                j["Target"]   = m.volume.container_path;
                j["ReadOnly"] = m.volume.read_only;
            } else {
                j["Type"]     = "bind";
                j["Source"]   = m.bind.host_path;
                j["Target"]   = m.bind.container_path;
                j["ReadOnly"] = m.bind.read_only;
            }
            mounts.push_back(j);
        }
        hc["Mounts"] = mounts;
    }

    body["HostConfig"] = std::move(hc);
    return body;
}
} // namespace detail

struct CreateResult {
    std::string id;
    std::string warnings;        // joined `Warnings[]` from daemon, or empty
};

struct WaitResult {
    int          exit_code = -1;
    std::string  error;          // daemon-side error message (empty = no err)
};

// A bare exec handle — we don't yet have a streaming attach (the proxy
// doesn't expose /exec/{id}/start in our whitelist). Returned id is
// suitable for a future commit's exec_capture helper, or for operators
// debugging stuck containers via curl from the host.
struct ExecResult {
    std::string id;
};

// ────────────────────────────────────────────────────────────────────────────
//  Client
// ────────────────────────────────────────────────────────────────────────────

class Client {
public:
    // Construct from URL string. May throw DockerConfigError.
    explicit Client(const std::string& url,
                    std::int32_t default_timeout_ms = 30'000)
        : endpoint_(detail::parse_endpoint_url(url)) {
        if (default_timeout_ms < 1)
            throw DockerConfigError("docker client timeout_ms must be >= 1");
        endpoint_.timeout_ms = default_timeout_ms;
    }

    // Construct from already-parsed Endpoint. Constructor itself never
    // throws (validation happens at Endpoint::parse time).
    explicit Client(Endpoint ep)
        : endpoint_(std::move(ep)) {
        if (endpoint_.timeout_ms < 1)
            endpoint_.timeout_ms = 30'000;
    }

    const Endpoint& endpoint() const noexcept { return endpoint_; }

    // ── Health / introspection ──
    //
    // `ping()` is the only non-throwing member. It is consumed by
    // `make_docker_probe` below for /api/v1/health; if it threw, the
    // probe layer would have to wrap every call in try/catch.
    bool ping() noexcept {
        try {
            auto r = do_request("GET", "/_ping", /*body=*/nullptr,
                                /*expect_json=*/false);
            return r.status == 200 || r.status == 204;
        } catch (...) {
            return false;
        }
    }

    nlohmann::json info() {
        auto r = do_request("GET", "/info", nullptr, /*expect_json=*/true);
        return r.body;
    }

    nlohmann::json version() {
        auto r = do_request("GET", "/version", nullptr, /*expect_json=*/true);
        return r.body;
    }

    // ── SPEC-mandated 4 + exec (the 5 white-listed ops) ──
    //
    // We call these `create`, `start`, `wait`, `kill`, `remove` to
    // match the docker CLI vocabulary that every reader knows. The
    // underlying HTTP path is /containers/...

    CreateResult create(const CreateOptions& opts) {
        if (opts.image.empty())
            throw DockerConfigError("docker create: image is required");
        nlohmann::json body = detail::build_create_body(opts);
        auto r = do_request("POST", "/containers/create",
                            &body, /*expect_json=*/true);

        CreateResult out;
        if (r.body.is_object() && r.body.contains("Id")) {
            out.id = r.body["Id"].get<std::string>();
        } else if (!r.raw.empty()) {
            // Defensive: daemon returned 2xx but we can't find Id.
            // Surface that as an Http error so the caller sees it
            // instead of a silently-empty id.
            throw DockerHttpError(r.status, "/containers/create", r.raw);
        }
        if (r.body.is_object() && r.body.contains("Warnings") &&
            r.body["Warnings"].is_array()) {
            std::ostringstream os;
            for (const auto& w : r.body["Warnings"]) {
                if (!os.str().empty()) os << "; ";
                if (w.is_string()) os << w.get<std::string>();
            }
            out.warnings = os.str();
        }
        return out;
    }

    void start(const std::string& id) {
        if (id.empty())
            throw DockerConfigError("docker start: container id is empty");
        std::string path = "/containers/" + id + "/start";
        // 204 No Content is the expected success response. Body is
        // empty, so don't ask do_request to parse JSON.
        auto r = do_request("POST", path, /*body=*/{}, /*expect_json=*/false);
        if (r.status != 204 && r.status != 200) {
            // do_request throws on 4xx/5xx already, but stay explicit
            // so a future schema change doesn't silently pass.
            throw DockerHttpError(r.status, path, r.raw);
        }
    }

    WaitResult wait(const std::string& id, std::int32_t timeout_ms) {
        if (id.empty())
            throw DockerConfigError("docker wait: container id is empty");
        if (timeout_ms < 1)
            throw DockerConfigError("docker wait: timeout_ms must be >= 1");

        std::string path = "/containers/" + id + "/wait";
        // condition=not-running — wait until the container exits.
        // Without this the proxy would block the connection forever
        // and a stuck container looks like a slow daemon.
        std::vector<std::pair<std::string, std::string>> qs{
            {"condition", "not-running"}
        };
        std::string qstr = detail::build_query(qs);
        if (!qstr.empty()) path += "?" + qstr;

        auto r = do_request("POST", path, /*body=*/{},
                            /*expect_json=*/true,
                            /*timeout_ms=*/timeout_ms);
        WaitResult out;
        if (r.body.is_object() && r.body.contains("StatusCode")) {
            if (r.body["StatusCode"].is_number_integer()) {
                out.exit_code = r.body["StatusCode"].get<int>();
            }
        }
        if (r.body.is_object() && r.body.contains("Error") &&
            r.body["Error"].is_string()) {
            out.error = r.body["Error"].get<std::string>();
        }
        return out;
    }

    void kill(const std::string& id) {
        if (id.empty())
            throw DockerConfigError("docker kill: container id is empty");
        std::string path = "/containers/" + id + "/kill";
        // Default SIGKILL is what warm_pool / scheduler want when a
        // judge goes off the rails; we don't expose signal selection
        // because the proxy configuration didn't whitelist query
        // params there either and we keep it boring.
        auto r = do_request("POST", path, /*body=*/{}, /*expect_json=*/false);
        if (r.status != 204 && r.status != 200) {
            throw DockerHttpError(r.status, path, r.raw);
        }
    }

    void remove(const std::string& id,
                bool force = true,
                bool remove_volumes = false) {
        if (id.empty())
            throw DockerConfigError("docker remove: container id is empty");
        std::string path = "/containers/" + id;
        std::vector<std::pair<std::string, std::string>> qs{
            {"force", force          ? "true" : "false"},
            {"v",     remove_volumes ? "true" : "false"},
        };
        std::string qstr = detail::build_query(qs);
        if (!qstr.empty()) path += "?" + qstr;

        auto r = do_request("DELETE", path, /*body=*/{}, /*expect_json=*/false);
        if (r.status != 204 && r.status != 200) {
            throw DockerHttpError(r.status, path, r.raw);
        }
    }

    // Inspect a single container (used by warm_pool / debug paths). The
    // proxy whitelist may or may not expose GET /containers/{id}/json;
    // the SPEC doesn't require it for the core ops but it falls out
    // for free given CONTAINERS=1.
    nlohmann::json inspect(const std::string& id) {
        if (id.empty())
            throw DockerConfigError("docker inspect: container id is empty");
        std::string path = "/containers/" + id + "/json";
        auto r = do_request("GET", path, nullptr, /*expect_json=*/true);
        return r.body;
    }

    // Logs — returns the last `tail_bytes` of (stdout+stderr) for the
    // given container. Used by judge_scheduler to surface a more
    // informative error_message than the envelope exposes when the
    // judge.sh stdout is malformed JSON.
    //
    // NOTE: tecnativa proxy exposes this through GET + CONTAINERS=1.
    // If the call fails, return an empty string and log WARN so the
    // log volume from a hot loop stays bounded.
    std::string logs(const std::string& id,
                     bool stdout_ = true,
                     bool stderr_ = true,
                     int tail_bytes = 16 * 1024) {
        if (id.empty()) return {};
        std::vector<std::pair<std::string, std::string>> qs{
            {"stdout",     stdout_    ? "true" : "false"},
            {"stderr",     stderr_    ? "true" : "false"},
            {"timestamps", "false"},
            {"follow",     "false"},
            {"tail",       std::to_string(tail_bytes)},
        };
        std::string path = "/containers/" + id + "/logs";
        std::string qstr = detail::build_query(qs);
        if (!qstr.empty()) path += "?" + qstr;

        try {
            auto r = do_request("GET", path, nullptr,
                                /*expect_json=*/false);
            if (r.status == 200) {
                // v1.2.50: the Docker Engine JSON logs endpoint
                // returns a MULTIPLEXED stream when stdout/stderr are
                // both enabled and the container was created without
                // TTY. Each frame has an 8-byte header:
                //   byte 0    : STREAM_TYPE (0 stdin, 1 stdout, 2 stderr)
                //   bytes 1-3 : 0
                //   bytes 4-7 : SIZE (uint32 big-endian)
                // followed by SIZE bytes of payload. The judge.sh
                // output JSON is on stdout (type=1); callers ask for
                // one stream at a time (stdout XOR stderr), so we
                // walk the framed bytes and concatenate only the
                // frames matching the requested stream. Non-framed
                // responses (some docker versions, or when only
                // stdout/stderr is requested and the other stream
                // is empty) bypass the loop and return raw.
                //
                // Found and fixed v1.2.50: parse_judge_result_json
                // walked log lines looking for a leading '{' but the
                // 8-byte prefix didn't match — every judge was marked
                // SE ("judge produced no parseable result JSON")
                // despite stdout containing exactly one JSON line.
                return detail::strip_docker_log_frames(r.raw, stdout_, stderr_);
            }
        } catch (const std::exception& e) {
            try {
                LOG_WARN("docker logs failed",
                         {{"container_id", id},
                          {"error",        e.what()}});
            } catch (...) {
                // logging must never flip a benign null-log into a crash
            }
        }
        return {};
    }

    // Lower-level: create a one-shot exec instance. The proxy exposes
    // POST /containers/{id}/exec, but the *attach* half (POST
    // /exec/{id}/start) is gated by EXEC_START which is *not* set in
    // our whitelist. We return the id so a future commit can
    // implement streaming attach (or so operators can `curl` directly
    // from the host using the same exec id).
    ExecResult create_exec(const std::string& container_id,
                           const std::vector<std::string>& cmd) {
        if (container_id.empty())
            throw DockerConfigError("docker create_exec: container id empty");
        if (cmd.empty())
            throw DockerConfigError("docker create_exec: cmd is empty");
        std::string path = "/containers/" + container_id + "/exec";
        nlohmann::json body{
            {"Cmd",          cmd},
            {"AttachStdout", false},
            {"AttachStderr", false},
            {"AttachStdin",  false},
            {"Tty",          false},
            {"Detach",       false},
        };
        auto r = do_request("POST", path, &body, /*expect_json=*/true);
        ExecResult out;
        if (r.body.is_object() && r.body.contains("Id")) {
            out.id = r.body["Id"].get<std::string>();
        } else {
            throw DockerHttpError(r.status, path, r.raw);
        }
        return out;
    }

private:
    // Result of a single HTTP round-trip. `raw` is the body verbatim
    // (for empty/204/no-Content-Length responses), `body` is the
    // parsed JSON if `expect_json` was true and the parser liked it.
    struct HttpResult {
        int            status   = 0;
        nlohmann::json body;          // null if not requested / unparsable
        std::string    raw;
    };

    // Single low-level call. `method` is uppercase. `path` is appended
    // to Endpoint::base to form the URL. `body` (optional) is sent as
    // application/json. `expect_json` controls parsing: if true and
    // the response body is empty/non-JSON, we synthesize an empty
    // object (rather than throwing) so callers like `start()` which
    // expect 204 stay ergonomic.
    //
    // Throws DockerTimeoutError on connect/read failure, DockerHttpError
    // on any non-2xx response.
    HttpResult do_request(const std::string& method,
                          const std::string& path,
                          const nlohmann::json* body,
                          bool expect_json,
                          std::int32_t timeout_ms = -1) {
        if (timeout_ms < 0) timeout_ms = endpoint_.timeout_ms;
        const std::string url    = endpoint_.to_url();
        const std::string full   = endpoint_.join(path);

        // httplib::Client documents that a single Client instance is
        // not thread-safe for concurrent use; creating a fresh one
        // per call avoids that, and is cheap relative to the docker
        // round-trip (and far cheaper than a g++ invocation).
        httplib::Client cli(url);
        int sec  = timeout_ms / 1000;
        int usec = (timeout_ms % 1000) * 1000;
        cli.set_connection_timeout(sec, usec);
        cli.set_read_timeout(sec, usec);
        cli.set_write_timeout(sec, usec);
        cli.set_keep_alive(false);
        cli.set_tcp_nodelay(true);

        httplib::Result rc;
        httplib::Headers headers;
        std::string body_str;
        if (body != nullptr) body_str = body->dump();

        if (method == "GET") {
            if (body != nullptr)
                throw DockerConfigError("GET cannot carry a body: " + path);
            rc = cli.Get(full, headers);
        } else if (method == "POST") {
            headers.emplace("Content-Type", "application/json");
            if (body == nullptr) {
                rc = cli.Post(full, headers);
            } else {
                rc = cli.Post(full, headers, body_str,
                              "application/json");
            }
        } else if (method == "DELETE") {
            if (body != nullptr)
                throw DockerConfigError("DELETE cannot carry a body: " + path);
            rc = cli.Delete(full, headers);
        } else {
            throw DockerConfigError("unsupported HTTP method: " + method);
        }

        if (!rc) {
            // httplib returns a null Result on connect / read / write
            // failure. `Result::error()` reports *what* failed; we
            // stringify it for the operator log so they see "Could
            // not establish connection" vs. "Connection timed out"
            // without needing a debugger.
            throw DockerTimeoutError(
                "docker " + method + " " + full + " failed: " +
                httplib::to_string(rc.error()));
        }

        HttpResult out;
        out.status = rc->status;
        out.raw    = rc->body;
        if (expect_json) {
            if (rc->body.empty()) {
                out.body = nlohmann::json::object();
            } else {
                try {
                    out.body = nlohmann::json::parse(rc->body);
                } catch (...) {
                    // Treat malformed JSON as empty object; let the
                    // caller inspect `raw` if it really wants the bytes.
                    out.body = nlohmann::json::object();
                }
            }
        }
        if (out.status < 200 || out.status >= 300) {
            throw DockerHttpError(out.status, full, rc->body);
        }
        return out;
    }

    Endpoint endpoint_;
};

// ────────────────────────────────────────────────────────────────────────────
//  Health probe factory
//
//  Wires `Client::ping()` into the existing HealthService so
//  /api/v1/health flips to 503 when the Docker proxy goes away. Caller
//  must own the client (e.g. a member of the JudgeScheduler); we
//  capture by pointer and tolerate lifetime ordering — if `client` is
//  null we report down (same contract as make_db_probe with a null pool).
// ────────────────────────────────────────────────────────────────────────────

inline HealthService::Probe make_docker_probe(Client* client) {
    return [client]() -> ProbeResult {
        ProbeResult r;
        if (client == nullptr) {
            r.ok     = false;
            r.detail = "no docker client configured";
            return r;
        }
        if (client->ping()) {
            r.ok     = true;
            r.detail = "docker proxy reachable";
            return r;
        }
        r.ok     = false;
        r.detail = "docker proxy unreachable";
        return r;
    };
}

// ────────────────────────────────────────────────────────────────────────────
//  Config bridge — turn a JudgeConfig into a Client.
//
//  Pulls DOCKER_SOCKET_URL / default timeouts from the JudgeConfig when
//  the caller doesn't have a separate URL string handy. Empty URL is
//  treated as "leave docker unconfigured" rather than throwing, so a
//  dev machine without docker can still run the test binary.
// ────────────────────────────────────────────────────────────────────────────

inline std::unique_ptr<Client> make_client_from_config(
    const JudgeConfig& cfg,
    std::int32_t default_timeout_ms = 30'000)
{
    if (cfg.docker_socket_url.empty()) return nullptr;
    return std::make_unique<Client>(cfg.docker_socket_url,
                                    default_timeout_ms);
}

} // namespace docker
} // namespace litecode

// SPDX-License-Identifier: MIT
// LiteCode-CPP — AppContext health factory (v1.2.48)
//
// Defines litecode::build_health_deps(). This is the one TU that
// pulls in src/routes/system_routes.h to construct a HealthService
// and register probes against the deps built by build_db_deps and
// build_judge_deps. main.cpp just stores the returned service into
// AppContext.health and passes it to register_health_routes.

#include "app_context_deps.h"
#include "judge/docker_client.h"
#include "judge/judge_scheduler.h"
#include "judge/warm_pool.h"
#include "routes/system_routes.h"

namespace litecode {

HealthDeps build_health_deps(const DbDeps&    db,
                             const JudgeDeps& judge,
                             std::function<ProbeResult()> /*docker_probe_unused*/) {
    HealthDeps out;
    out.health = std::make_unique<HealthService>();

    out.health->register_probe("uptime", make_uptime_probe());

    if (db.pool) {
        out.health->register_probe("db", make_db_probe(db.pool.get()));
    }
    if (judge.docker_client) {
        out.health->register_probe(
            "docker",
            make_docker_probe(judge.docker_client.get()));
    }
    if (judge.scheduler) {
        out.health->register_probe(
            "judge_queue",
            judge::JudgeScheduler::make_probe(judge.scheduler.get()));
    }
    if (judge.warm_pool) {
        out.health->register_probe(
            "warm_pool",
            judge::WarmPool::make_probe(judge.warm_pool.get()));
    }
    return out;
}

} // namespace litecode
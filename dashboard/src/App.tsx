import { useCallback, useState, type ReactNode } from "react";
import {
  Activity,
  ArrowDownUp,
  Boxes,
  Cpu,
  Database,
  Layers3,
  LayoutGrid,
  List,
  Menu,
  RefreshCw,
  X,
} from "lucide-react";
import {
  Link,
  NavLink,
  Route,
  Routes,
  useLocation,
  useParams,
} from "react-router-dom";
import {
  api,
  statuses,
  type Filters,
  type Job,
  type Queue,
  type Runtime,
  type Workers,
} from "./api";
import { Button } from "@/components/ui/button";
import { Card } from "@/components/ui/card";
import { useEventStream, useLive } from "./live";
import { useQuery } from "./query";

import { MetricsPage } from "./MetricsPage";

const navigation = [
  { to: "/", name: "Overview", icon: LayoutGrid },
  { to: "/jobs", name: "Jobs", icon: List },
  { to: "/queue", name: "Queue", icon: Layers3 },
  { to: "/workers", name: "Workers", icon: Cpu },
  { to: "/metrics", name: "Metrics", icon: Activity },
  { to: "/events", name: "Events", icon: Activity },
];

function Snapshot<T>({
  result,
  children,
  label,
}: {
  result: { data?: T; error?: string } | undefined;
  children: (data: T) => ReactNode;
  label: string;
}) {
  if (result?.error)
    return (
      <div className="notice error" role="alert">
        {result.error}
      </div>
    );
  if (result?.data === undefined)
    return (
      <div className="notice" role="status">
        Loading {label}…
      </div>
    );
  return children(result.data);
}

function Badge({ value }: { value: string }) {
  return (
    <span className={"badge " + value.toLowerCase()}>
      {value.toLowerCase()}
    </span>
  );
}

function Facts({ items }: { items: [string, ReactNode][] }) {
  return (
    <dl className="facts">
      {items.map(([label, value]) => (
        <div key={label}>
          <dt>{label}</dt>
          <dd>{value}</dd>
        </div>
      ))}
    </dl>
  );
}

function QueueFacts({ data }: { data: Queue }) {
  return (
    <Facts
      items={[
        ["Ready to run", data.ready],
        ["Waiting for retry", data.delayed],
        ["Queue state", <Badge value={data.closed ? "Closed" : "Open"} />],
      ]}
    />
  );
}

function WorkerFacts({ data }: { data: Workers }) {
  return (
    <Facts
      items={[
        ["Pool state", <Badge value={data.state} />],
        ["Configured workers", data.configured_workers],
      ]}
    />
  );
}

function duration(seconds: number) {
  const units = [
    [86400, "d"],
    [3600, "h"],
    [60, "m"],
    [1, "s"],
  ] as const;
  return (
    units
      .map(([size, suffix]) => {
        const part = Math.floor(seconds / size);
        seconds %= size;
        return part ? part + suffix : "";
      })
      .filter(Boolean)
      .join(" ") || "0s"
  );
}

function RuntimeFacts({ data }: { data: Runtime }) {
  return (
    <Facts
      items={[
        ["Server", <Badge value={data.server.state} />],
        ["Engine", <Badge value={data.engine.state} />],
        ["Uptime", duration(data.server.uptime_seconds)],
        [
          "Submission",
          data.engine.accepting_jobs ? "Accepting jobs" : "Closed",
        ],
        ["Configured Engine workers", data.engine.configured_workers],
        ["JobQ version", data.version.jobq],
      ]}
    />
  );
}

function useLiveJobs(snapshot?: { baseline?: string; generation?: number }) {
  const events = useLive(state => state.events);
  const generation = useLive(state => state.generation);
  return (job: Job): Job => {
    if (generation !== snapshot?.generation) return job;
    const baseline = snapshot.baseline ? BigInt(snapshot.baseline) : 0n;
    const update = events.find(event => event.data.job_id === job.id &&
      BigInt(event.sequence) > baseline)?.data;
    if (!update || update.attempts_made < job.attempts_made) return job;
    if (update.attempts_made === job.attempts_made &&
        job.status !== update.previous_status && job.status !== update.status) return job;
    return { ...job, status: update.status, attempts_made: update.attempts_made };
  };
}

function JobTable({ jobs, snapshot }: { jobs: Job[]; snapshot?: { baseline?: string; generation?: number } }) {
  const liveJob = useLiveJobs(snapshot);
  if (!jobs.length)
    return (
      <div className="notice">
        <Database size={22} />
        No jobs match this view.
      </div>
    );
  return (
    <div
      className="table-wrap"
      tabIndex={0}
      role="region"
      aria-label="Job records"
    >
      <table>
        <thead>
          <tr>
            {[
              "Job ID",
              "Type",
              "Status",
              "Priority",
              "Attempts",
              "Payload preview",
            ].map((name) => (
              <th key={name} scope="col">
                {name}
              </th>
            ))}
          </tr>
        </thead>
        <tbody>
          {jobs.map(liveJob).map((job) => (
            <tr key={job.id}>
              <td>
                <Link className="mono job-link" to={`/jobs/${job.id}`}>
                  #{job.id}
                </Link>
              </td>
              <td>
                {job.type}
                {job.type_truncated ? "…" : ""}
              </td>
              <td>
                <Badge value={job.status} />
              </td>
              <td>{job.priority.toLowerCase()}</td>
              <td className="mono">{job.attempts_made}</td>
              <td>
                <code className="preview">{job.payload_preview || "—"}</code>
              </td>
            </tr>
          ))}
        </tbody>
      </table>
    </div>
  );
}

function Feed({ compact = false }: { compact?: boolean }) {
  const events = useLive((state) => state.events);
  const connection = useLive((state) => state.connection);
  const shown = compact ? events.slice(0, 5) : events;
  if (!shown.length)
    return (
      <div className="notice">
        <Activity size={22} />
        {connection === "connected"
          ? "No lifecycle events since connecting."
          : "Waiting for the event stream."}
      </div>
    );
  return (
    <ol className="feed">
      {shown.map(({ sequence, data }) => (
        <li key={sequence}>
          <div>
            <Link className="mono job-link" to={`/jobs/${data.job_id}`}>
              #{data.job_id}
            </Link>
            <span className="subtle">Sequence {sequence}</span>
          </div>
          <div className="flow">
            <Badge value={data.previous_status} />
            <span aria-label="changed to">→</span>
            <Badge value={data.status} />
          </div>
          <span className="subtle">Attempt {data.attempts_made}</span>
        </li>
      ))}
    </ol>
  );
}

async function loadOverview(signal: AbortSignal) {
  const [
    totalPage,
    pending,
    running,
    completed,
    failed,
    queue,
    workers,
    runtime,
  ] = await Promise.all([
    api.jobs({ limit: 1, offset: 0 }, signal),
    api.jobs({ limit: 1, offset: 0, status: "PENDING" }, signal),
    api.jobs({ limit: 1, offset: 0, status: "RUNNING" }, signal),
    api.jobs({ limit: 1, offset: 0, status: "COMPLETED" }, signal),
    api.jobs({ limit: 1, offset: 0, status: "FAILED" }, signal),
    api.queue(signal),
    api.workers(signal),
    api.runtime(signal),
  ]);
  const total = totalPage.pagination.total;
  const recent = total
    ? (
        await api.jobs({ limit: 5, offset: Math.max(0, total - 5) }, signal)
      ).data.slice().reverse()
    : [];
  return {
    total,
    pending: pending.pagination.total,
    running: running.pagination.total,
    completed: completed.pagination.total,
    failed: failed.pagination.total,
    queue,
    workers,
    runtime,
    recent,
  };
}

function Overview() {
  const result = useQuery(loadOverview);
  return (
    <>
      <div className="heading">
        <h1>Overview</h1>
        <p>Inspect the current state of your JobQ instance.</p>
      </div>
      <Snapshot result={result} label="overview">
        {(data) => (
          <>
            <div className="stats">
              {([
                ["Retained jobs", data.total, Boxes],
                ["Pending jobs", data.pending, Layers3],
                ["Running jobs", data.running, Activity],
                ["Completed jobs", data.completed, List],
                ["Failed jobs", data.failed, X],
                ["Ready queue", data.queue.ready, Database],
                ["Delayed queue", data.queue.delayed, ArrowDownUp],
                ["Configured workers", data.workers.configured_workers, Cpu],
              ] as const).map(([label, value, StatIcon]) => {
                return (
                  <Card key={String(label)} className="stat">
                    <div>
                      <span>{String(label)}</span>
                      <StatIcon size={16} />
                    </div>
                    <strong>{String(value)}</strong>
                  </Card>
                );
              })}
            </div>
            <p className="snapshot-note">
              Counts are sampled separately and may reflect nearby instants.
            </p>
            <div className="panels">
              <Card>
                <h2>Runtime</h2>
                <RuntimeFacts data={data.runtime} />
              </Card>
              <Card>
                <h2>Queue</h2>
                <QueueFacts data={data.queue} />
              </Card>
              <Card>
                <h2>Worker pool</h2>
                <WorkerFacts data={data.workers} />
              </Card>
            </div>
            <Card>
              <div className="panel-heading">
                <h2>Recent jobs</h2>
                <Link to="/jobs">View all jobs</Link>
              </div>
              <JobTable jobs={data.recent} snapshot={result} />
            </Card>
          </>
        )}
      </Snapshot>
      <Card>
        <div className="panel-heading">
          <h2>Live activity</h2>
          <Link to="/events">View events</Link>
        </div>
        <Feed compact />
      </Card>
    </>
  );
}

function Jobs() {
  const [filters, setFilters] = useState<Filters>({ limit: 20, offset: 0 });
  const [typeInput, setTypeInput] = useState("");
  const load = useCallback(
    (signal: AbortSignal) => api.jobs(filters, signal),
    [filters],
  );
  const result = useQuery(load);
  return (
    <>
      <div className="heading">
        <h1>Jobs</h1>
        <p>Browse and inspect retained job records.</p>
      </div>
      <Card>
        <form
          className="filters"
          onSubmit={(event) => {
            event.preventDefault();
            setFilters((current) => ({
              ...current,
              type: typeInput || undefined,
              offset: 0,
            }));
          }}
        >
          <label>
            Status
            <select
              value={filters.status ?? ""}
              onChange={(event) => {
                const status = statuses.find(
                  (value) => value === event.target.value,
                );
                setFilters((current) => ({ ...current, status, offset: 0 }));
              }}
            >
              <option value="">All statuses</option>
              {statuses.map((status) => (
                <option key={status}>{status}</option>
              ))}
            </select>
          </label>
          <label>
            Exact job type
            <input
              value={typeInput}
              maxLength={256}
              placeholder="Filter by type"
              onChange={(event) => setTypeInput(event.target.value)}
            />
          </label>
          <Button type="submit">Apply type filter</Button>
          <label className="page-size">
            Page size
            <select
              value={filters.limit}
              onChange={(event) =>
                setFilters((current) => ({
                  ...current,
                  limit: Number(event.target.value),
                  offset: 0,
                }))
              }
            >
              {[10, 20, 50, 100].map((size) => (
                <option key={size}>{size}</option>
              ))}
            </select>
          </label>
        </form>
        <Snapshot result={result} label="jobs">
          {(page) => {
            const { offset, limit, total } = page.pagination;
            return (
              <>
                <JobTable jobs={page.data} snapshot={result} />
                <div className="pagination">
                  <span>
                    {page.data.length
                      ? `${offset + 1}–${offset + page.data.length} of ${total}`
                      : `0 shown · ${total} retained`}
                  </span>
                  <Button
                    disabled={offset === 0}
                    onClick={() =>
                      setFilters((current) => ({
                        ...current,
                        offset: Math.max(0, offset - limit),
                      }))
                    }
                  >
                    Previous
                  </Button>
                  <Button
                    disabled={offset + limit >= total}
                    onClick={() =>
                      setFilters((current) => ({
                        ...current,
                        offset: offset + limit,
                      }))
                    }
                  >
                    Next
                  </Button>
                </div>
              </>
            );
          }}
        </Snapshot>
      </Card>
    </>
  );
}

function JobDetail() {
  const { id = "" } = useParams();
  const load = useCallback((signal: AbortSignal) => api.job(id, signal), [id]);
  const result = useQuery(load);
  const liveJob = useLiveJobs(result);
  return (
    <>
      <div className="heading">
        <Link to="/jobs">All jobs</Link>
        <h1 className="mono">Job #{id}</h1>
      </div>
      <Snapshot result={result} label="job">
        {(job) => (
          <>
            <div className="panels detail">
              <Card>
                <h2>Execution</h2>
                <Facts
                  items={[
                    ["Job ID", <code>{job.id}</code>],
                    ["Type", job.type + (job.type_truncated ? "…" : "")],
                    ["Status", <Badge value={liveJob(job).status} />],
                    ["Priority", job.priority],
                    ["Attempts", liveJob(job).attempts_made],
                  ]}
                />
              </Card>
              <Card>
                <h2>Retry policy</h2>
                <Facts
                  items={[
                    ["Maximum retries", job.retry.max_retries],
                    ["Initial delay", job.retry.initial_delay_ms + " ms"],
                    ["Maximum delay", job.retry.max_delay_ms + " ms"],
                    ["Multiplier", job.retry.multiplier],
                  ]}
                />
              </Card>
            </div>
            <Card>
              <h2>Payload preview</h2>
              <Facts
                items={[
                  ["Encoding", job.payload_preview_encoding],
                  ["Original size", job.payload_bytes + " bytes"],
                  ["Preview truncated", job.payload_truncated ? "Yes" : "No"],
                ]}
              />
              <pre>{job.payload_preview || "(empty preview)"}</pre>
            </Card>
          </>
        )}
      </Snapshot>
    </>
  );
}

function QueuePage() {
  const result = useQuery(api.queue);
  return (
    <>
      <div className="heading">
        <h1>Queue</h1>
        <p>Waiting jobs ready to execute or delayed for retry.</p>
      </div>
      <Card>
        <h2>Queue snapshot</h2>
        <Snapshot result={result} label="queue">
          {(data) => <QueueFacts data={data} />}
        </Snapshot>
      </Card>
    </>
  );
}

function WorkersPage() {
  const result = useQuery(api.workers);
  return (
    <>
      <div className="heading">
        <h1>Workers</h1>
        <p>Inspect the Engine’s configured worker pool.</p>
      </div>
      <Card>
        <h2>Worker pool</h2>
        <Snapshot result={result} label="workers">
          {(data) => <WorkerFacts data={data} />}
        </Snapshot>
        <p className="snapshot-note">
          The API exposes aggregate pool state and configured count. Individual
          identities and active or idle counts are unavailable.
        </p>
      </Card>
    </>
  );
}

function EventsPage() {
  return (
    <>
      <div className="heading">
        <h1>Events</h1>
        <p>Job lifecycle updates from the current WebSocket connection.</p>
      </div>
      <Card>
        <h2>Live stream</h2>
        <Feed />
      </Card>
    </>
  );
}

export function App() {
  useEventStream();
  const health = useQuery(api.ready);
  const connection = useLive((state) => state.connection);
  const refresh = useLive((state) => state.manualRefresh);
  const { pathname } = useLocation();
  const [menu, setMenu] = useState(false);
  const page =
    navigation.find(
      (item) =>
        item.to === pathname ||
        (item.to !== "/" && pathname.startsWith(item.to + "/")),
    )?.name ?? "JobQ";
  return (
    <div className="shell">
      <a href="#main" className="skip">
        Skip to content
      </a>
      <aside className={menu ? "sidebar expanded" : "sidebar"}>
        <Link className="brand" to="/" onClick={() => setMenu(false)}>
          <span>
            <Layers3 size={21} />
          </span>
          JobQ
        </Link>
        <nav aria-label="Main navigation">
          {navigation.map(({ to, name, icon: Icon }) => (
            <NavLink
              key={to}
              to={to}
              end={to === "/"}
              onClick={() => setMenu(false)}
            >
              <Icon size={18} />
              {name}
            </NavLink>
          ))}
        </nav>
      </aside>
      <div className="workspace">
        <header className="topbar">
          <Button
            className="menu-toggle"
            aria-label={menu ? "Close navigation" : "Open navigation"}
            aria-expanded={menu}
            onClick={() => setMenu((value) => !value)}
          >
            {menu ? <X size={19} /> : <Menu size={19} />}
          </Button>
          <strong>{page}</strong>
          <div className="connections">
            <span
              title={health?.error}
              className={
                health?.data?.data.status === "ready"
                  ? "indicator up"
                  : "indicator"
              }
            >
              Server{" "}
              {health?.data?.data.status === "ready"
                ? "ready"
                : health?.error
                  ? "unavailable"
                  : "checking"}
            </span>
            <span
              className={
                connection === "connected" ? "indicator up" : "indicator"
              }
            >
              Events {connection}
            </span>
          </div>
          <Button aria-label="Refresh snapshots" onClick={refresh}>
            <RefreshCw size={15} />
            <span className="refresh-label">Refresh</span>
          </Button>
        </header>
        <main id="main" tabIndex={-1}>
          <Routes>
            <Route path="/" element={<Overview />} />
            <Route path="/jobs" element={<Jobs />} />
            <Route path="/jobs/:id" element={<JobDetail />} />
            <Route path="/queue" element={<QueuePage />} />
            <Route path="/workers" element={<WorkersPage />} />
            <Route path="/metrics" element={<MetricsPage />} />
            <Route path="/events" element={<EventsPage />} />
            <Route
              path="*"
              element={
                <div className="notice">
                  Page not found. <Link to="/">Return to overview</Link>
                </div>
              }
            />
          </Routes>
        </main>
      </div>
    </div>
  );
}

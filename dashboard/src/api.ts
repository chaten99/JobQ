import axios from "axios";

export const statuses = [
  "PENDING",
  "RUNNING",
  "COMPLETED",
  "FAILED",
  "CANCELLED",
] as const;
export type Status = (typeof statuses)[number];
export interface Job {
  id: string;
  type: string;
  type_truncated: boolean;
  status: Status;
  priority: "LOW" | "NORMAL" | "HIGH";
  attempts_made: number;
  retry: {
    max_retries: number;
    initial_delay_ms: number;
    max_delay_ms: number;
    multiplier: number;
  };
  payload_preview: string;
  payload_preview_encoding: string;
  payload_bytes: number;
  payload_truncated: boolean;
}
export interface Metrics {
  schema_version: 1;
  counters: Record<"submitted_jobs" | "started_executions" | "completed_executions" | "failed_executions" | "scheduled_retries" | "finished_executions", string>;
  performance: {
    total_execution_time_ns: string;
    average_execution_time_ns: string;
    elapsed_time_ns: string;
    finished_executions_per_second: number | null;
  };
}

export interface Queue {
  ready: number;
  delayed: number;
  closed: boolean;
}
export interface Workers {
  state: string;
  configured_workers: number;
}
export interface Runtime {
  server: { state: string; uptime_seconds: number };
  engine: {
    state: string;
    accepting_jobs: boolean;
    configured_workers: number;
  };
  version: { jobq: string };
}
export interface JobPage {
  data: Job[];
  pagination: { total: number; limit: number; offset: number };
}
export interface Filters {
  limit: number;
  offset: number;
  status?: Status;
  type?: string;
}

const baseURL = import.meta.env.VITE_JOBQ_API_BASE_URL || "";
export const http = axios.create({ baseURL, timeout: 8000 });

export function errorMessage(error: unknown) {
  if (
    !axios.isAxiosError<{ error?: { code?: string; message?: string } }>(error)
  )
    return "The snapshot could not be loaded.";
  const response = error.response;
  if (!response)
    return "Could not reach JobQ. Check that the HTTP server is running.";
  const detail = response.data?.error;
  if (response.status >= 500 && response.status !== 503)
    return "JobQ could not complete this request.";
  if (typeof detail?.message === "string")
    return detail.code ? `${detail.message} (${detail.code})` : detail.message;
  return (
    (
      {
        400: "Invalid request.",
        404: "The requested record or route does not exist.",
        422: "JobQ rejected the job type.",
        503: "The JobQ Engine is unavailable.",
      } as Record<number, string>
    )[response.status] ?? "The request failed."
  );
}

async function resource<T>(path: string, signal: AbortSignal) {
  return (await http.get<{ data: T }>(path, { signal })).data.data;
}

export const api = {
  metrics: (signal: AbortSignal) => resource<Metrics>("/api/v1/metrics", signal),
  live: (signal: AbortSignal) =>
    http.get<{ status: "ok" }>("/health/live", { signal }),
  ready: (signal: AbortSignal) =>
    http.get<{ status: "ready" }>("/health/ready", { signal }),
  jobs: async (params: Filters, signal: AbortSignal) =>
    (await http.get<JobPage>("/api/v1/jobs", { params, signal })).data,
  job: (id: string, signal: AbortSignal) =>
    resource<Job>(`/api/v1/jobs/${encodeURIComponent(id)}`, signal),
  runtime: (signal: AbortSignal) =>
    resource<Runtime>("/api/v1/runtime", signal),
  workers: (signal: AbortSignal) =>
    resource<Workers>("/api/v1/workers", signal),
  queue: (signal: AbortSignal) => resource<Queue>("/api/v1/queue", signal),
};

export function socketUrl(
  origin: string,
  apiBase = baseURL,
  override = import.meta.env.VITE_JOBQ_WS_URL as string | undefined,
) {
  if (override) return new URL(override).href;
  const url = new URL(`${apiBase.replace(/\/$/, "")}/api/v1/events`, origin);
  url.protocol = url.protocol === "https:" ? "wss:" : "ws:";
  return url.href;
}

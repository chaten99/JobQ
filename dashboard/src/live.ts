import { useEffect } from "react";
import { create } from "zustand";
import { socketUrl, statuses, type Status } from "./api";

export interface JobEvent {
  event: "job.status_changed";
  sequence: string;
  data: {
    job_id: string;
    previous_status: Status;
    status: Status;
    attempts_made: number;
  };
}

const uint64 = (value: unknown): value is string =>
  typeof value === "string" &&
  /^[1-9][0-9]{0,19}$/.test(value) &&
  BigInt(value) <= 18446744073709551615n;

const status = (value: unknown): value is Status =>
  statuses.some((item) => item === value);

export function parseEvent(message: unknown): JobEvent | undefined {
  if (typeof message !== "string") return;
  try {
    const value: unknown = JSON.parse(message);
    if (
      !value ||
      typeof value !== "object" ||
      !("event" in value) ||
      value.event !== "job.status_changed" ||
      !("sequence" in value) ||
      !uint64(value.sequence) ||
      !("data" in value)
    )
      return;
    const data = value.data;
    if (
      !data ||
      typeof data !== "object" ||
      !("job_id" in data) ||
      !uint64(data.job_id) ||
      !("previous_status" in data) ||
      !status(data.previous_status) ||
      !("status" in data) ||
      !status(data.status) ||
      !("attempts_made" in data) ||
      typeof data.attempts_made !== "number" ||
      !Number.isSafeInteger(data.attempts_made) ||
      data.attempts_made < 0
    )
      return;
    return {
      event: value.event,
      sequence: value.sequence,
      data: {
        job_id: data.job_id,
        previous_status: data.previous_status,
        status: data.status,
        attempts_made: data.attempts_made,
      },
    };
  } catch {
    return;
  }
}

type Connection = "connecting" | "connected" | "reconnecting" | "disconnected";
interface LiveState {
  connection: Connection;
  events: JobEvent[];
  revision: number;
  manualRevision: number;
  manualRefresh: () => void;
  generation: number;
  refresh: () => void;
}
export const useLive = create<LiveState>((set) => ({
  connection: "disconnected",
  events: [],
  revision: 0,
  manualRevision: 0,
  manualRefresh: () => set((state) => ({ revision: state.revision + 1, manualRevision: state.manualRevision + 1 })),
  generation: 0,
  refresh: () => set((state) => ({ revision: state.revision + 1 })),
}));

export function useEventStream() {
  useEffect(() => {
    let socket: WebSocket | undefined;
    let stopped = false;
    let failures = 0;
    let last = 0n;
    let refreshTimer: ReturnType<typeof setTimeout> | undefined;
    let stabilityTimer: ReturnType<typeof setTimeout> | undefined;
    let retryTimer: ReturnType<typeof setTimeout>;
    const refresh = () => useLive.getState().refresh();
    const retry = () => {
      if (stopped) return;
      if (useLive.getState().connection === "connected") refresh();
      clearTimeout(stabilityTimer);
      useLive.setState({ connection: "reconnecting" });
      const delay = Math.min(30_000, 750 * 2 ** Math.min(failures++, 6));
      retryTimer = setTimeout(connect, delay * (0.75 + Math.random() * 0.25));
    };
    const connect = () => {
      if (stopped) return;
      try {
        socket = new WebSocket(socketUrl(window.location.origin));
      } catch {
        retry();
        return;
      }
      socket.onopen = () => {
        last = 0n;
        stabilityTimer = setTimeout(() => {
          failures = 0;
        }, 10_000);
        useLive.setState((state) => ({ connection: "connected", events: [], generation: state.generation + 1 }));
        refresh();
      };
      socket.onmessage = (message) => {
        const event = parseEvent(message.data);
        if (!event || BigInt(event.sequence) <= last) return;
        last = BigInt(event.sequence);
        useLive.setState((state) => ({
          events: [event, ...state.events].slice(0, 100),
        }));
        if (refreshTimer === undefined) {
          refreshTimer = setTimeout(() => {
            refreshTimer = undefined;
            refresh();
          }, 400);
        }
      };
      socket.onclose = retry;
      socket.onerror = () => socket?.close();
    };
    useLive.setState({ connection: "connecting" });
    retryTimer = setTimeout(connect, 0);
    return () => {
      stopped = true;
      clearTimeout(retryTimer);
      clearTimeout(refreshTimer);
      clearTimeout(stabilityTimer);
      if (socket) {
        socket.onopen =
          socket.onclose =
          socket.onmessage =
          socket.onerror =
            null;
        socket.close();
      }
      useLive.setState({ connection: "disconnected" });
    };
  }, []);
}

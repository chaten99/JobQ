import { act, cleanup, fireEvent, render, screen, waitFor } from '@testing-library/react'
import { MemoryRouter } from 'react-router-dom'
import { AxiosError, type AxiosResponse } from 'axios'
import { afterEach, beforeEach, expect, test, vi } from 'vitest'
import { App } from './App'
import { api, errorMessage, http, socketUrl, type Job } from './api'
import { parseEvent, useLive } from './live'

class MockSocket {
  static sockets: MockSocket[] = []
  onopen: (() => void) | null = null
  onclose: (() => void) | null = null
  onerror: (() => void) | null = null
  onmessage: ((event: { data: string }) => void) | null = null
  close = vi.fn()
  constructor(public url: string) { MockSocket.sockets.push(this) }
}

const id = '18446744073709551615'
const job: Job = {
  id, type: 'ECHO', type_truncated: false, status: 'RUNNING', priority: 'NORMAL', attempts_made: 1,
  retry: { max_retries: 1, initial_delay_ms: 10, max_delay_ms: 100, multiplier: 2 },
  payload_preview: 'sample', payload_preview_encoding: 'escaped_bytes', payload_bytes: 42, payload_truncated: true,
}
const message = (sequence: string, status = 'COMPLETED') => JSON.stringify({
  event: 'job.status_changed', sequence,
  data: { job_id: id, previous_status: 'RUNNING', status, attempts_made: 1 },
})

function open(path: string) {
  return render(<MemoryRouter initialEntries={[path]}><App /></MemoryRouter>)
}

beforeEach(() => {
  MockSocket.sockets = []
  vi.stubGlobal('WebSocket', MockSocket)
  useLive.setState({ connection: 'disconnected', events: [], revision: 0 })
})

afterEach(() => {
  cleanup()
  vi.restoreAllMocks()
  vi.unstubAllGlobals()
  vi.useRealTimers()
})

test('serializes jobs filters and keeps uint64 IDs and pagination metadata', async () => {
  const adapter = http.defaults.adapter
  try {
    http.defaults.adapter = async config => {
      const url = http.getUri(config)
      expect(url).toBe('/api/v1/jobs?limit=20&offset=40&status=RUNNING&type=A%26B')
      return { data: { data: [job], pagination: { limit: 20, offset: 40, total: 61 } }, config,
        status: 200, statusText: 'OK', headers: {} }
    }
    const page = await api.jobs({ limit: 20, offset: 40, status: 'RUNNING', type: 'A&B' }, new AbortController().signal)
    expect(page.data[0].id).toBe(id)
    expect(page.pagination.total).toBe(61)
  } finally { http.defaults.adapter = adapter }
})

test('surfaces a 503 error envelope and redacts unexpected server errors', () => {
  const response = { status: 503, data: { error: { code: 'ENGINE_UNAVAILABLE', message: 'Starting up' } } } as AxiosResponse
  expect(errorMessage(new AxiosError('503', undefined, undefined, undefined, response)))
    .toBe('Starting up (ENGINE_UNAVAILABLE)')
  expect(errorMessage(new AxiosError('500', undefined, undefined, undefined, { ...response, status: 500 })))
    .toBe('JobQ could not complete this request.')
})

test('overview shows loading, then errors without fabricating empty counts', async () => {
  vi.spyOn(api, 'ready').mockRejectedValue(new AxiosError('offline'))
  vi.spyOn(api, 'jobs').mockRejectedValue(new AxiosError('offline'))
  vi.spyOn(api, 'workers').mockResolvedValue({ state: 'running', configured_workers: 4 })
  vi.spyOn(api, 'queue').mockResolvedValue({ ready: 2, delayed: 1, closed: false })
  vi.spyOn(api, 'runtime').mockRejectedValue(new AxiosError('offline'))
  open('/')
  expect(screen.getByText('Loading overview…')).toBeTruthy()
  expect(await screen.findByRole('alert')).toBeTruthy()
  expect(screen.queryByText('Retained jobs')).toBeNull()
})

test('filters, pages, and opens details with the exact job ID', async () => {
  vi.spyOn(api, 'ready').mockRejectedValue(new AxiosError('offline'))
  const jobs = vi.spyOn(api, 'jobs').mockImplementation(async params => ({
    data: [job], pagination: { offset: params.offset, limit: params.limit, total: 50 },
  }))
  const detail = vi.spyOn(api, 'job').mockResolvedValue(job)
  open('/jobs')
  await screen.findByRole('link', { name: '#' + id })
  fireEvent.change(screen.getByLabelText('Status'), { target: { value: 'RUNNING' } })
  fireEvent.change(screen.getByLabelText('Exact job type'), { target: { value: 'ECHO' } })
  fireEvent.click(screen.getByRole('button', { name: 'Apply type filter' }))
  await waitFor(() => expect(jobs).toHaveBeenLastCalledWith({ status: 'RUNNING', type: 'ECHO', offset: 0, limit: 20 }, expect.any(AbortSignal)))
  fireEvent.click(await screen.findByRole('button', { name: 'Next' }))
  await waitFor(() => expect(jobs.mock.lastCall?.[0].offset).toBe(20))
  fireEvent.click(await screen.findByRole('link', { name: '#' + id }))
  expect(await screen.findByText('42 bytes')).toBeTruthy()
  expect(screen.getByText('escaped_bytes')).toBeTruthy()
  expect(detail).toHaveBeenCalledWith(id, expect.any(AbortSignal))
})

test('shows real queue and worker values and toggles navigation', async () => {
  vi.spyOn(api, 'ready').mockRejectedValue(new AxiosError('offline'))
  vi.spyOn(api, 'workers').mockResolvedValue({ state: 'running', configured_workers: 4 })
  vi.spyOn(api, 'queue').mockResolvedValue({ ready: 2, delayed: 1, closed: true })
  open('/queue')
  expect(await screen.findByText('closed')).toBeTruthy()
  const menu = screen.getByRole('button', { name: 'Open navigation' })
  fireEvent.click(menu)
  expect(menu.getAttribute('aria-expanded')).toBe('true')
  fireEvent.click(screen.getByRole('link', { name: 'Workers' }))
  expect(await screen.findByText('4')).toBeTruthy()
  expect(menu.getAttribute('aria-expanded')).toBe('false')
})

test('resolves same-origin and explicit WebSocket URLs', () => {
  expect(socketUrl('http://localhost:5173', '')).toBe('ws://localhost:5173/api/v1/events')
  expect(socketUrl('https://jobq.test', '')).toBe('wss://jobq.test/api/v1/events')
  expect(socketUrl('http://localhost', 'https://api.jobq.test')).toBe('wss://api.jobq.test/api/v1/events')
  expect(socketUrl('http://localhost', '', 'wss://events.jobq.test/stream')).toBe('wss://events.jobq.test/stream')
})

test('ignores malformed, duplicate and stale messages, refreshes jobs, reconnects and cleans up', async () => {
  vi.useFakeTimers()
  vi.spyOn(api, 'ready').mockRejectedValue(new AxiosError('offline'))
  const jobs = vi.spyOn(api, 'jobs').mockResolvedValue({ data: [job], pagination: { limit: 20, offset: 0, total: 1 } })
  const app = open('/jobs')
  await act(async () => { await vi.advanceTimersByTimeAsync(0) })
  const first = MockSocket.sockets[0]
  expect(MockSocket.sockets).toHaveLength(1)
  await act(async () => first.onopen?.())
  jobs.mockResolvedValue({ data: [{ ...job, status: 'COMPLETED' }], pagination: { limit: 20, offset: 0, total: 1 } })
  await act(async () => {
    for (const data of ['{', '{}', message('9007199254740993'), message('9007199254740993'), message('9007199254740992')]) {
      first.onmessage?.({ data })
    }
    await vi.advanceTimersByTimeAsync(400)
  })
  expect(useLive.getState().events.map(event => event.sequence)).toEqual(['9007199254740993'])
  expect(screen.getByText('completed')).toBeTruthy()
  expect(parseEvent(message('1', 'UNKNOWN'))).toBeUndefined()
  const requests = jobs.mock.calls.length
  await act(async () => {
    first.onclose?.()
    await vi.advanceTimersByTimeAsync(1000)
  })
  const second = MockSocket.sockets[1]
  await act(async () => second.onopen?.())
  expect(jobs.mock.calls.length).toBeGreaterThan(requests)
  await act(async () => second.onmessage?.({ data: message('1') }))
  expect(useLive.getState().events[0].sequence).toBe('1')
  app.unmount()
  expect(second.close).toHaveBeenCalledOnce()
  expect(second.onmessage).toBeNull()
})

test("overview loads recent jobs from the end of the ascending list", async () => {
  vi.spyOn(api, "ready").mockRejectedValue(new AxiosError("offline"));
  vi.spyOn(api, "workers").mockResolvedValue({ state: "running", configured_workers: 4 });
  vi.spyOn(api, "queue").mockResolvedValue({ ready: 0, delayed: 0, closed: false });
  vi.spyOn(api, "runtime").mockResolvedValue({
    server: { state: "running", uptime_seconds: 65 },
    engine: { state: "running", accepting_jobs: true, configured_workers: 4 },
    version: { jobq: "0.1.0" },
  });
  const jobs = vi.spyOn(api, "jobs").mockImplementation(async ({ limit, offset, status }) => ({
    data: offset ? [job] : [],
    pagination: { limit, offset, total: status ? 0 : 60 },
  }));
  open("/");
  expect(await screen.findByRole("link", { name: "#" + id })).toBeTruthy();
  expect(screen.getByText("1m 5s")).toBeTruthy();
  expect(screen.getByText("Waiting for the event stream.")).toBeTruthy();
  expect(jobs).toHaveBeenCalledWith({ limit: 5, offset: 55 }, expect.any(AbortSignal));
});

test("shows a genuine empty job result", async () => {
  vi.spyOn(api, "ready").mockRejectedValue(new AxiosError("offline"));
  vi.spyOn(api, "jobs").mockResolvedValue({
    data: [], pagination: { limit: 20, offset: 0, total: 0 },
  });
  open("/jobs");
  expect(await screen.findByText("No jobs match this view.")).toBeTruthy();
  expect((screen.getByRole("button", { name: "Next" }) as HTMLButtonElement).disabled).toBe(true);
});

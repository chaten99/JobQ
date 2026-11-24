import { act, cleanup, render, screen } from '@testing-library/react';
import { afterEach, expect, test, vi } from 'vitest';
import { api, http, type Metrics } from './api';
import { useLive } from './live';
import { count, durationNs, nextRates, successShare } from './metrics';
import { MetricsPage } from './MetricsPage';

function sample(finished = '0', elapsed = '1000000000'): Metrics {
  return { schema_version: 1, counters: {
    submitted_jobs: finished, started_executions: finished, completed_executions: finished,
    failed_executions: '0', scheduled_retries: '0', finished_executions: finished,
  }, performance: {
    elapsed_time_ns: elapsed, total_execution_time_ns: '0', average_execution_time_ns: '0',
    finished_executions_per_second: 0,
  } };
}

afterEach(() => {
  cleanup();
  vi.restoreAllMocks();
  vi.useRealTimers();
});

test('metrics resource preserves the envelope values as decimal strings', async () => {
  const get = vi.spyOn(http, 'get').mockResolvedValue({ data: { data: sample('18446744073709551615') } });
  const signal = new AbortController().signal;
  expect((await api.metrics(signal)).counters.finished_executions).toBe('18446744073709551615');
  expect(get).toHaveBeenCalledWith('/api/v1/metrics', { signal });
});

test('formats large counters and durations without losing precision', () => {
  expect(count('18446744073709551615')).toBe('18,446,744,073,709,551,615');
  expect(durationNs('6230456')).toBe('6.23 ms');
  expect(durationNs('18446744073709551615')).toBe('18,446,744,073.709 s');
  expect(successShare(sample())).toBe('Unavailable');
  const data = sample('3');
  data.counters.completed_executions = '2';
  expect(successShare(data)).toBe('66.6%');
});

test('derives bounded rates, skips equal times, and clears history on reset', () => {
  const history = Array<number>(30).fill(10);
  const first = sample('9007199254740993');
  const second = sample('9007199254740994', '3000000000');
  const updated = nextRates(first, second, history);
  expect(updated).toHaveLength(30);
  expect(updated.at(-1)).toBe(0.5);
  expect(nextRates(second, second, updated)).toBe(updated);
  expect(nextRates(second, sample(), updated)).toEqual([]);
});

test('polls only while mounted and visible and reconciles manual refresh and reconnect', async () => {
  vi.useFakeTimers();
  let hidden = false;
  vi.spyOn(document, 'hidden', 'get').mockImplementation(() => hidden);
  const fetchMetrics = vi.spyOn(api, 'metrics').mockResolvedValue(sample());
  const view = render(<MetricsPage />);
  expect(screen.getByRole('status').textContent).toContain('Loading metrics');
  await act(async () => vi.advanceTimersByTimeAsync(10_000));
  expect(fetchMetrics).toHaveBeenCalledTimes(2);
  hidden = true;
  document.dispatchEvent(new Event('visibilitychange'));
  await act(async () => vi.advanceTimersByTimeAsync(30_000));
  expect(fetchMetrics).toHaveBeenCalledTimes(2);
  hidden = false;
  await act(async () => { document.dispatchEvent(new Event('visibilitychange')); });
  expect(fetchMetrics).toHaveBeenCalledTimes(3);
  await act(async () => useLive.getState().manualRefresh());
  expect(fetchMetrics).toHaveBeenCalledTimes(4);
  await act(async () => useLive.setState(state => ({ generation: state.generation + 1 })));
  expect(fetchMetrics).toHaveBeenCalledTimes(5);
  await act(async () => useLive.getState().refresh());
  expect(fetchMetrics).toHaveBeenCalledTimes(5);
  fetchMetrics.mockImplementation(() => new Promise(() => {}));
  act(() => useLive.getState().manualRefresh());
  const pending = fetchMetrics.mock.lastCall?.[0];
  view.unmount();
  expect(pending?.aborted).toBe(true);
  await vi.advanceTimersByTimeAsync(40_000);
  expect(fetchMetrics).toHaveBeenCalledTimes(6);
});

test('refresh failure preserves the last successful snapshot', async () => {
  const fetchMetrics = vi.spyOn(api, 'metrics').mockResolvedValue(sample('1'));
  render(<MetricsPage />);
  await screen.findByText('Submitted jobs');
  fetchMetrics.mockRejectedValue(new Error('Unavailable'));
  await act(async () => useLive.getState().manualRefresh());
  expect(screen.getByRole('alert').textContent).toContain('Showing the last successful snapshot');
  expect(screen.getByText('Submitted jobs')).toBeTruthy();
});

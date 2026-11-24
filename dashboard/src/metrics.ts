import type { Metrics } from './api';

export function count(value: string) {
  return BigInt(value).toLocaleString('en-US');
}

export function durationNs(value: string) {
  const ns = BigInt(value);
  for (const [unit, scale] of [['s', 1000000000n], ['ms', 1000000n], ['µs', 1000n]] as const) {
    if (ns >= scale) {
      const fraction = ((ns % scale) * 1000n / scale).toString().padStart(3, '0').replace(/0+$/, '');
      return `${count(String(ns / scale))}${fraction ? '.' + fraction : ''} ${unit}`;
    }
  }
  return `${count(value)} ns`;
}

export function successShare(metrics: Metrics) {
  const finished = BigInt(metrics.counters.finished_executions);
  if (!finished) return 'Unavailable';
  const tenths = BigInt(metrics.counters.completed_executions) * 1000n / finished;
  return `${tenths / 10n}.${tenths % 10n}%`;
}

export function nextRates(previous: Metrics | undefined, current: Metrics, history: number[]) {
  if (!previous) return history;
  const elapsed = BigInt(current.performance.elapsed_time_ns) - BigInt(previous.performance.elapsed_time_ns);
  const finished = BigInt(current.counters.finished_executions) - BigInt(previous.counters.finished_executions);
  if (elapsed < 0n || finished < 0n ||
      (Object.keys(current.counters) as (keyof Metrics['counters'])[]).some(
        key => BigInt(current.counters[key]) < BigInt(previous.counters[key]))) return [];
  if (elapsed === 0n) return history;
  const milliRate = finished * 1000000000000n / elapsed;
  return [...history, Number(milliRate) / 1000].slice(-30);
}

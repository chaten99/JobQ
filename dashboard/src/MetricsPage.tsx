import { useEffect, useRef, useState } from 'react';
import { api, errorMessage, type Metrics } from './api';
import { Card } from '@/components/ui/card';
import { useLive } from './live';
import { count, durationNs, nextRates, successShare } from './metrics';

const counters = [
  ['submitted_jobs', 'Submitted jobs'], ['started_executions', 'Started executions'],
  ['completed_executions', 'Completed executions'], ['failed_executions', 'Failed executions'],
  ['scheduled_retries', 'Scheduled retries'], ['finished_executions', 'Finished executions'],
] as const;

export function MetricsPage() {
  const [snapshot, setSnapshot] = useState<Metrics>();
  const [rates, setRates] = useState<number[]>([]);
  const [received, setReceived] = useState<Date>();
  const [error, setError] = useState<string>();
  const previous = useRef<Metrics>(undefined);

  useEffect(() => {
    let active = true;
    let request: AbortController | undefined;
    let timer: ReturnType<typeof setTimeout> | undefined;
    let pending = false;
    const refresh = () => {
      clearTimeout(timer);
      if (!active || document.hidden) return;
      if (request) { pending = true; return; }
      request = new AbortController();
      const signal = request.signal;
      void api.metrics(signal).then(data => {
        if (signal.aborted) return;
        const baseline = previous.current;
        setRates(history => nextRates(baseline, data, history));
        previous.current = data;
        setSnapshot(data);
        setReceived(new Date());
        setError(undefined);
      }, failure => {
        if (!signal.aborted) setError(errorMessage(failure));
      }).finally(() => {
        request = undefined;
        if (!active || document.hidden) return;
        if (pending) { pending = false; refresh(); }
        else timer = setTimeout(refresh, 10_000);
      });
    };
    const visibility = () => {
      clearTimeout(timer);
      if (document.hidden) { pending = false; request?.abort(); }
      else refresh();
    };
    document.addEventListener('visibilitychange', visibility);
    const unsubscribe = useLive.subscribe((state, old) => {
      if (state.generation !== old.generation || state.manualRevision !== old.manualRevision) refresh();
    });
    refresh();
    return () => {
      active = false;
      clearTimeout(timer);
      request?.abort();
      unsubscribe();
      document.removeEventListener('visibilitychange', visibility);
    };
  }, []);

  const peak = Math.max(1, ...rates);
  const points = rates.map((rate, index) => `${index * 560 / Math.max(1, rates.length - 1)},${115 - rate * 105 / peak}`);
  return <>
    <div className="heading"><h1>Metrics</h1><p>Engine counters and execution performance.</p></div>
    {error && <div className="notice error" role="alert">{snapshot ? 'Refresh failed. Showing the last successful snapshot. ' : ''}{error}</div>}
    {!snapshot && !error && <div className="notice" role="status">Loading metrics…</div>}
    {snapshot && <>
      {received && <p className="snapshot-note">Last received in this browser <time dateTime={received.toISOString()}>{received.toLocaleTimeString()}</time></p>}
      <div className="stats">{counters.map(([key, label]) => <Card key={key} className="stat">
        <div>{label}</div><strong className="break-all">{count(snapshot.counters[key])}</strong>
      </Card>)}</div>
      {snapshot.counters.finished_executions === '0' && <p>No execution attempts have finished.</p>}
      <div className="panels detail">
        <Card><h2>Execution performance</h2><dl className="facts">{[
          ['Total execution time', durationNs(snapshot.performance.total_execution_time_ns)],
          ['Average execution time', durationNs(snapshot.performance.average_execution_time_ns)],
          ['Metrics elapsed time', durationNs(snapshot.performance.elapsed_time_ns)],
          ['Lifetime finished attempts / second', snapshot.performance.finished_executions_per_second?.toLocaleString('en-US', { maximumFractionDigits: 3 }) ?? 'Unavailable'],
          ['Successful share of finished attempts', successShare(snapshot)],
        ].map(([label, value]) => <div key={label}><dt>{label}</dt><dd>{value}</dd></div>)}</dl></Card>
        <Card><h2>Observed finished attempts / second</h2>
          <p className="snapshot-note">Recent intervals sampled in this browser. History clears when you leave this page.</p>
          {rates.length === 0 ? <div className="notice">Waiting for two successive snapshots.</div> :
            <figure><svg viewBox="0 0 560 120" role="img" aria-label="Observed finished execution attempts per second">
              <line x1="0" y1="115" x2="560" y2="115" stroke="#465163" />
              <polyline points={points.join(' ')} fill="none" stroke="#65d7ed" strokeWidth="2" />
              {points.map((point, index) => {
                const [cx, cy] = point.split(',');
                return <circle key={index} cx={cx} cy={cy} r="3" fill="#65d7ed" />;
              })}
            </svg><figcaption>Latest interval {rates.at(-1)?.toLocaleString('en-US', { maximumFractionDigits: 3 })} attempts/s · {rates.length} {rates.length === 1 ? 'sample' : 'samples'}</figcaption></figure>}
        </Card>
      </div>
    </>}
  </>;
}

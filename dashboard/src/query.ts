import { useEffect, useState } from "react";
import { errorMessage } from "./api";
import { useLive } from "./live";

export function useQuery<T>(load: (signal: AbortSignal) => Promise<T>) {
  const revision = useLive((state) => state.revision);
  const [result, setResult] = useState<{
    load: typeof load;
    data?: T;
    error?: string;
    baseline?: string;
    generation?: number;
  }>();
  useEffect(() => {
    const controller = new AbortController();
    const { events, generation } = useLive.getState();
    const baseline = events[0]?.sequence;
    load(controller.signal).then(
      (data) => {
        if (!controller.signal.aborted)
          setResult({ load, data, baseline, generation });
      },
      (error) => {
        if (!controller.signal.aborted)
          setResult({ load, error: errorMessage(error) });
      },
    );
    return () => controller.abort();
  }, [load, revision]);
  return result?.load === load ? result : undefined;
}

import { useCallback, useEffect, useRef, useState } from 'react'
import type { PerformanceHubApi, ServiceHealth } from './api/client'

export function useHealth(api: PerformanceHubApi) {
  const [state, setState] = useState<{ health?: ServiceHealth; error?: Error; loading: boolean }>({ loading: true })
  const request = useRef<AbortController | undefined>(undefined)
  const refresh = useCallback(() => {
    request.current?.abort()
    const controller = new AbortController()
    request.current = controller
    setState({ loading: true })
    // Check identity as well as cancellation: adapters may ignore AbortSignal.
    const current = () => request.current === controller && !controller.signal.aborted
    void Promise.resolve().then(() => api.health(controller.signal)).then(
      (health) => { if (current()) setState({ health, loading: false }) },
      (error: unknown) => {
        if (current()) setState({ error: error instanceof Error ? error : new Error('Health request failed'), loading: false })
      },
    )
  }, [api])

  useEffect(() => {
    // A new API client starts a new request and clears the previous client's result.
    // eslint-disable-next-line react-hooks/set-state-in-effect
    refresh()
    return () => { request.current?.abort(); request.current = undefined }
  }, [refresh])
  return { ...state, refresh }
}

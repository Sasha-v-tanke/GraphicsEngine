export type Route = { path: string; label: string; enabled: boolean }

export const routes: Route[] = [
  { path: '/benchmarks', label: 'Benchmarks', enabled: true },
  { path: '/run-presets', label: 'Run Presets', enabled: true },
  { path: '/runs', label: 'Runs', enabled: true },
  { path: '/compare', label: 'Compare', enabled: false },
  { path: '/metrics', label: 'Metrics', enabled: false },
  { path: '/dashboards', label: 'Dashboards', enabled: false },
  { path: '/profiler', label: 'Profiler', enabled: false },
]

export function normalizeRoute(pathname: string) {
  return routes.some((route) => route.path === pathname && route.enabled) ? pathname : '/benchmarks'
}

export type ServiceHealth = {
  status: 'ok' | 'degraded' | 'unavailable'
  version?: string
  detail?: string
}

export class ApiError extends Error {
  constructor(message: string, readonly status?: number) {
    super(message)
    this.name = 'ApiError'
  }
}

export interface PerformanceHubApi {
  health(signal?: AbortSignal): Promise<ServiceHealth>
}

function isHealth(value: unknown): value is ServiceHealth {
  if (typeof value !== 'object' || value === null || !('status' in value)) return false
  return typeof value.status === 'string'
    && ['ok', 'degraded', 'unavailable'].includes(value.status)
    && (!('version' in value) || typeof value.version === 'string')
    && (!('detail' in value) || typeof value.detail === 'string')
}

export function createApi(baseUrl = ''): PerformanceHubApi {
  return {
    async health(signal) {
      const response = await fetch(`${baseUrl}/health`, { headers: { Accept: 'application/json' }, signal })
      if (!response.ok) throw new ApiError(`Health request failed (${response.status})`, response.status)
      const payload: unknown = await response.json()
      if (!isHealth(payload)) throw new ApiError('Health response does not match the API contract')
      return payload
    },
  }
}

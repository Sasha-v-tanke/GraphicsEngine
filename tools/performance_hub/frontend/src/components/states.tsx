import type { ReactNode } from 'react'

export function LoadingState({ label = 'Loading…' }: { label?: string }) {
  return <p className="state" role="status">{label}</p>
}

export function EmptyState({ title, detail }: { title: string; detail: string }) {
  return <section className="state" aria-labelledby="empty-title"><h2 id="empty-title">{title}</h2><p>{detail}</p></section>
}

export function ErrorState({ error, retry }: { error: Error; retry?: () => void }) {
  return <section className="state error" role="alert"><h2>Unable to load data</h2><p>{error.message}</p>{retry && <button type="button" onClick={retry}>Try again</button>}</section>
}

export function Notice({ children }: { children: ReactNode }) {
  return <p className="notice">{children}</p>
}

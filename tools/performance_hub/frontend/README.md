# Performance Hub frontend

The frontend is an optional host tool. It has no dependency on the installed GraphicsEngine SDK, Vulkan, ImGui, or Editor.

## Local development

```sh
npm install
npm run dev
```

The Vite development server proxies `/health` and `/api` to the local Performance Hub backend at `127.0.0.1:8000`. A production build expects the host service to serve `dist/` and to preserve SPA fallback for direct routes.

## API boundary

Production code calls only `src/api/client.ts`; it does not contain demo data. Fixtures and mocked API implementations are limited to tests. The initial C/H01 contract surface is `GET /health`; endpoints introduced by later backend stages must extend this typed boundary rather than making ad-hoc `fetch` calls in components.

## Components

`components/states.tsx` supplies loading, empty, and API error states. `components/controls.tsx` supplies keyboard-native search, select, checkbox-tree, and pagination controls. Navigation, date filters, and search state are encoded in the URL so direct links and reloads preserve them.

`components/DataTable.tsx` renders typed columns and stable row keys, an accessible caption and column headers, an empty row, horizontal scrolling, and client-side pagination. Pass `rows`, `columns: TableColumn<Row>[]`, `rowKey`, and optional positive `pageSize` (default 20). Pages are clamped when rows shrink. Fixtures belong in tests; the component does not fetch or fabricate records.

Health responses validate every declared field at runtime. Each refresh aborts the previous request and ignores its completion even if an adapter ignores cancellation. Changing clients or unmounting also cancels outstanding work. Supported URL ranges are `7d`, `30d`, and `all`; invalid values normalize to `7d` while preserving other query parameters.

import { defineConfig } from 'vite';

// Relative base so the built site works from any static host or sub-path.
export default defineConfig({
  base: './',
  worker: { format: 'es' },
  build: { target: 'es2022' },
  test: { environment: 'node' },
});

import { fileURLToPath } from 'node:url'
import react from '@vitejs/plugin-react'
import tailwind from '@tailwindcss/vite'
import { defineConfig } from 'vitest/config'

export default defineConfig({
  plugins: [react(), tailwind()],
  resolve: { alias: { '@': fileURLToPath(new URL('./src', import.meta.url)) } },
  server: {
    proxy: {
      '/api': { target: 'http://127.0.0.1:8080', ws: true },
      '/health': 'http://127.0.0.1:8080',
    },
  },
  test: { environment: 'jsdom' },
})

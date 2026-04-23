/** @type {import('tailwindcss').Config} */
export default {
  content: ["./index.html", "./src/**/*.{ts,tsx}"],
  theme: {
    extend: {
      colors: {
        ink: "#0b0d13",
        panel: "#121723",
        panelmuted: "#1a1f2c",
        accent: "#67e8f9",
        accent2: "#a78bfa",
        ok: "#22c55e",
        warn: "#f59e0b",
        err: "#ef4444",
        muted: "#9ca3af",
      },
      fontFamily: {
        sans: [
          "InterVariable",
          "Inter",
          "Segoe UI",
          "system-ui",
          "sans-serif",
        ],
        mono: [
          "JetBrains Mono",
          "Cascadia Code",
          "Consolas",
          "Menlo",
          "monospace",
        ],
      },
    },
  },
  plugins: [],
};

\*\*Short answer:\*\* duplication can explain the teleport and snap issues very well. The “walking into the ceiling” issue is \*helped\* by duplication, but it likely also comes from how the \*\*generated\*\* slide movers apply motion — not from duplication alone.

\---

\#\#\# 1\. Sliding lifts you into the air / toward the ceiling

\*\*Partly related to duplication, but not the whole story.\*\*

What duplication does:  
\- In head/controller slide mode, \*\*both\*\* \`generated.\*\_sliding\` and \`builtin.locomotion.slide.\*\` run.  
\- Both call \`ctx.locomotion-\>translate(...)\` every frame → roughly \*\*double movement\*\*, and \*\*two collision sweeps\*\*.

What matters more for the climb:  
\- Built-in slide always asks for \*\*XZ-only\*\* motion (\`y \= 0\`).  
\- Generated slide does extra work: \`slide\_move(...)\` \*\*then\*\* \`translate(safe\_delta)\`.    
  \`translate()\` runs \*\*another\*\* \`slide\_character\` sweep. That sweep can return a \*\*small \+Y\*\* when the capsule slides along sloped/uneven collision (range geometry, fixtures, etc.). That Y is written into the rig and \*\*accumulates\*\*.  
\- Teleport doesn’t do this: it \*\*sets\*\* position from a landing, with a floor clamp — so no gradual climb.

So: duplication makes climbing worse / faster, but the core bug is “slide path can accumulate vertical motion.” Your default should be ground-following; jump/fly should be explicit mechanics, not accidental Y drift.

\---

\#\#\# 2\. Double teleport lines (second only when stick pushed forward)

\*\*Yes — this is almost certainly the duplication.\*\*

| Who | When it draws the arc |  
|-----|------------------------|  
| Built-in teleport | Stick \*\*magnitude\*\* \> \~0.5 (any direction) |  
| Generated teleport | Stick \*\*forward Y\*\* \> \~0.5 |

That matches what you saw:  
\- Push left/right (not fully forward) → mostly \*\*one\*\* line (built-in)  
\- Push straight forward → \*\*two\*\* lines (built-in \+ generated)

So this is not a designed “double teleport” feature. It’s \*\*two teleporters\*\* both aiming when forward is pressed.

\---

\#\#\# 3\. Black flicker on snap turn

\*\*Yes — strongly tied to the generated snap (and made worse by duplication).\*\*

\- \`generated.snap\_turn\` intentionally draws a short \*\*black comfort blink\*\* (\`ctx.draw-\>marker\` in front of the head) on each snap.  
\- Built-in snap has \*\*no blink\*\*, but also \*\*no latch\*\* — while you hold the stick, it can keep snapping every cooldown.

That matches your behavior:  
\- Flick left/right repeatedly → each release/re-press allows another generated snap → \*\*blink each time\*\*  
\- Hold the stick → generated snaps once (latches) \+ blinks once/twice, then \*\*builtin\*\* keeps turning without more blinks → continuous turn, flicker goes away

So the black screen is the generated comfort blink, not a random GPU glitch. Duplication means you can get \*\*extra turns\*\* (both handlers) on the first flick.


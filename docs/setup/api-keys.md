# API keys

The recommended setup uses **OpenAI** for every AI planning and code stage, plus **Gemini**, **Tripo3D**, and **Uthana**. Do not create a Claude key. Recent engine updates were tested with OpenAI, not Claude.

Some stages are ordinary LLM calls (the build spec, the asset list, and optimization preferences). If `ANTHROPIC_API_KEY` is empty, those calls use OpenAI. Most AI stages already go through the OpenAI Agents SDK and need `OPENAI_API_KEY` on their own.

Hands Only, with **no full-body avatar and no in-game NPCs**, needs three keys only: **OpenAI, Gemini, and Tripo3D**. Skip Uthana for that game. A Hands Only game that still has NPCs can still need Uthana.

Funding amounts for small, medium, and large prompts are in the [README](../../README.md#recommended-api-funding). Those are recommended balances to have available, not a bill for one game and not the price of creating a key. Add at least that much credit at each provider you use. Uthana funding does not apply to the three-key case.

Real keys go in a file named `.env` at the repository root. That file is git-ignored. `.env.example` is the template and must stay free of real keys. The examples below use placeholders.

```powershell
cd C:\path\to\Prompt-To-VR
copy .env.example .env
```

Open `.env` in an editor and set the keys. Restart the orchestrator after you change it.

```env
OPENAI_API_KEY=example-openai-key
GOOGLE_API_KEY=example-google-key
TRIPO_API_KEY=example_tripo-key
UTHANA_API_KEY=example-uthana-key
VCPKG_ROOT=C:\src\vcpkg
```

Leave `ANTHROPIC_API_KEY` empty. If PowerShell already has a Claude key in the environment, clearing `.env` is not enough. In PowerShell:

```powershell
echo $env:ANTHROPIC_API_KEY
```

If that prints a key, remove the user environment variable named `ANTHROPIC_API_KEY` (Windows search → Environment Variables), close the terminal, and start the orchestrator again from a new window.

A local check is enough: the names above are filled in `.env`, and `git status` does not list `.env` as a file to commit. You do not need to send a paid test request to finish setup.

## OpenAI

OpenAI plans the game, writes gameplay code, and covers the regular LLM stages when Claude is not configured.

1. Create or sign in to an account at [https://platform.openai.com/](https://platform.openai.com/). A ChatGPT subscription is a different product and does not fund the API.
2. Open [API keys](https://platform.openai.com/api-keys). Choose **Create new secret key**, copy it once, and store it yourself. The site shows the full secret only at creation. Official help: [Where do I find my OpenAI API key?](https://help.openai.com/en/articles/4936850-where-do-i-find-my-openai-api-key)
3. Open the API organization's [billing overview](https://platform.openai.com/settings/organization/billing/overview). New API accounts use prepaid credits. OpenAI's help says the minimum credit purchase is $5 and the setup screen defaults to $10. Turn off auto-reload if you do not want further automatic purchases. Help: [Prepaid API billing](https://help.openai.com/en/articles/8264644-setting-up-and-managing-prepaid-api-billing). The README's OpenAI column ($50 / $75, $100 / $150, or $200 / $300) is the balance recommended for this engine, which is more than that minimum.
4. Put the key in `.env` as `OPENAI_API_KEY`.

The engine calls `gpt-5.6-sol` for OpenAI stages. You do not pick a different model in the dashboard for the recommended setup.

## Gemini

Gemini makes the reference images that Tripo turns into meshes. The orchestrator reads `GOOGLE_API_KEY`. Put the key in that variable. `GEMINI_API_KEY` is not the name this app's settings load.

1. Sign in at [Google AI Studio](https://aistudio.google.com/).
2. Open the API keys page: [https://aistudio.google.com/app/apikey](https://aistudio.google.com/app/apikey). New accounts often get a default project and key after you accept the terms. Otherwise choose **Create API key**. Official steps: [Using Gemini API keys](https://aistudio.google.com/docs/api-key).
3. To buy more than the free tier, use **Set up billing** on the AI Studio keys or projects page. Google's getting-started note says the paid-tier prepay minimum is $5. The README Gemini amounts ($5, $10, or $25) are the recommended available budget. Steps: [Gemini getting started](https://aistudio.google.com/docs/get-started).
4. Put the key in `.env` as `GOOGLE_API_KEY`.

The image model the repo expects is `gemini-3.1-flash-image`. `.env.example` already sets `GEMINI_IMAGE_MODEL` to that. Leave it unless you know you need a different value.

## Tripo3D

Tripo builds the textured 3D objects and characters, and it can rig and animate characters that do not go through Uthana.

1. Sign in at the [Tripo developer console](https://developers.tripo3d.ai/).
2. Open [API keys](https://developers.tripo3d.ai/en/keys) and choose **Create Key**. Copy the key. Tripo shows it once. Steps: [Authentication](https://developers.tripo3d.ai/en/docs/authentication).
3. Add API credits in the console. Tripo bills in credits (their pricing page treats 1 credit as $0.01). The README Tripo amounts ($10, $25, or $50–75) are the recommended available budget. Billing explanation: [Tripo billing](https://developers.tripo3d.ai/en/docs/billing). The exact button label is on the signed-in console; use the control that adds API credits.
4. Put the key in `.env` as `TRIPO_API_KEY`.

Accounts on the China site need a different base URL. In `.env`:

```env
TRIPO_BASE_URL=https://openapi.tripo3d.com/v3
```

Leave `TRIPO_BASE_URL` empty for the international site (`https://openapi.tripo3d.ai/v3`). A 401 on the balance check usually means the key and the base URL are from different regions.

## Uthana

Uthana rigs and animates characters when the game needs that, including the full-body player avatar. Skip this section for the three-key Hands Only case with no avatar and no NPCs.

1. Create an account at [https://uthana.com/](https://uthana.com/).
2. Choose a plan that includes API access. Pricing: [https://uthana.com/pricing](https://uthana.com/pricing).
3. Open [account settings](https://uthana.com/app/account), generate an API key, and copy it. Official steps: [Uthana API key](https://uthana.com/docs/api/api-key).
4. In that same account page, open **Billing** and activate pay-as-you-go if you want wallet credits. Uthana's pricing doc says you top up the wallet from account settings and that generation spends those credits. The README Uthana amounts ($5, $10, or $20–25) are the recommended available budget. [Uthana API pricing](https://uthana.com/docs/api/pricing).
5. Put the key in `.env` as `UTHANA_API_KEY`.

## If a key does not work

| What you see | What to do |
| --- | --- |
| Stages still call Claude | `ANTHROPIC_API_KEY` is set in `.env` or in the Windows user environment. Clear it and start the orchestrator again. |
| Reference images fail immediately | `GOOGLE_API_KEY` is empty or still the placeholder. |
| Tripo returns 401 | The key does not match `TRIPO_BASE_URL` (international vs China). |
| Tripo or Uthana stops mid-run for credit | Add credits in that provider's console. The README amounts are a starting budget, not a cap the engine enforces. |
| `.env` shows up in git | Do not commit it. It is listed in `.gitignore`. If it was added anyway, remove it from the index without deleting your local copy. |

Back to the [setup index](README.md) or the [README](../../README.md). Model names and endpoints in more detail: [Provider setup](../provider-setup.md).

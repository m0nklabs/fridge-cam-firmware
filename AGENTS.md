## GitHub Actions runners (centrale pool van `m0nklabs`)

Dit project gebruikt de **centrale org-level self-hosted runners** van de
`m0nklabs`-organisatie, die OP DEZE SERVER draaien. Er zijn 4 runners
(`m0nklabs-runner-1` t/m `-4`) die voor álle projecten werken. **Voeg nooit een
aparte runner per project toe.**

- Host: `ai-kvm2`
- Labels (automatisch): `self-hosted`, `Linux`, `X64`
- Alle 4 runners zijn **GPU-capabel** (label `gpu`; 2× NVIDIA RTX op de host).
- Bron van waarheid & configuratie: de **publieke** repo
  `m0nklabs/github-action-runners` (zie README.md en AGENTS.md daar).

### Runners gebruiken in workflows

- Gewone job (geen GPU):
  ```yaml
  runs-on: [self-hosted, Linux]
  ```
- GPU-job:
  ```yaml
  runs-on: [self-hosted, Linux, gpu]
  ```

### GPU loopt SERIEEL (1 tegelijk)

Alle runners delen dezelfde GPU's. **Elke** GPU-job moet zijn zware commando's
wrappen met de centrale lock, zodat 2 jobs nooit op dezelfde kaarten concurreren:

```yaml
- name: Train
  run: /home/flip/github-action-runners/bin/gpu-run.sh <command>
```

### Generieke (reusable) workflows

Gebruik in plaats van kopiëren de generieke workflows uit
`m0nklabs/github-action-runners`: `python-ci`, `frontend-ci`, `go-ci`,
`rust-ci`, `gpu-ci`, `codeql-detect`. Roep alleen de workflows voor de talen die
dit project écht bevat.

```yaml
jobs:
  codeql:
    uses: m0nklabs/github-action-runners/.github/workflows/codeql-detect.yml@main
    secrets: inherit
```

### Regels

- **Geen runners per project toevoegen** — gebruik altijd de org-pool.
- **Geen GPU-werk zonder `gpu-run.sh`** — anders concurreren 2 jobs op dezelfde kaarten.
- **Geen secrets committen** in workflows.
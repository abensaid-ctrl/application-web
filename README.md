# application-web

## Deploiement sur Vercel

Ce projet est statique et peut etre deploye directement.

### Option 1: via l'interface Vercel (recommande)

1. Pousse ce depot sur GitHub.
2. Va sur https://vercel.com/new.
3. Importe le depot `application-web`.
4. Laisse les reglages par defaut (pas de build obligatoire).
5. Clique sur **Deploy**.

### Option 2: via la CLI Vercel

```bash
npm i -g vercel
vercel login
vercel
vercel --prod
```

### Notes

- Le fichier `vercel.json` redirige la racine `/` vers `3.html`.
- Tu peux donc garder les noms de fichiers actuels.
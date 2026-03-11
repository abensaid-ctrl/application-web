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

- `index.html` est maintenant le point d'entree principal pour Vercel.
- `3.html` est conserve pour compatibilite.
- En production, l'application fonctionne automatiquement en mode demo sans backend local.
- Si tu veux brancher un backend distant, ajoute `?api=https://ton-api/api` a l'URL.
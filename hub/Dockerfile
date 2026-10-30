FROM node:20-alpine
WORKDIR /app
COPY package.json server.js ./
COPY src ./src
ENV PORT=8787 HUB_DATA=/data/hub-data.json
VOLUME /data
EXPOSE 8787
USER node
CMD ["node", "server.js"]

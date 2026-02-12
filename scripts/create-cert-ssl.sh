openssl req -x509 -nodes -days 365 -newkey rsa:2048 \
-keyout key.pem -out cert.pem \
-subj "/C=US/ST=State/L=City/O=TheRetroCenter/CN=localhost"

mv cert.pem ./ssl/cert.pem
mv key.pem ./ssl/key.pem
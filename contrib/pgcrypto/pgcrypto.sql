
-- drop function digest(bytea, text);
-- drop function digest_exists(text);
-- drop function hmac(bytea, bytea, text);
-- drop function hmac_exists(text);
-- drop function crypt(text, text);
-- drop function gen_salt(text);
-- drop function gen_salt(text, int4);
-- drop function encrypt(bytea, bytea, text);
-- drop function decrypt(bytea, bytea, text);
-- drop function encrypt_iv(bytea, bytea, bytea, text);
-- drop function decrypt_iv(bytea, bytea, bytea, text);
-- drop function cipher_exists(text);



CREATE FUNCTION digest(bytea, text) RETURNS bytea
  AS '$libdir/pgcrypto',
  'pg_digest' LANGUAGE 'C';

CREATE FUNCTION digest_exists(text) RETURNS bool
  AS '$libdir/pgcrypto',
  'pg_digest_exists' LANGUAGE 'C';

CREATE FUNCTION hmac(bytea, bytea, text) RETURNS bytea
  AS '$libdir/pgcrypto',
  'pg_hmac' LANGUAGE 'C';

CREATE FUNCTION hmac_exists(text) RETURNS bool
  AS '$libdir/pgcrypto',
  'pg_hmac_exists' LANGUAGE 'C';

CREATE FUNCTION crypt(text, text) RETURNS text
  AS '$libdir/pgcrypto',
  'pg_crypt' LANGUAGE 'C';

CREATE FUNCTION gen_salt(text) RETURNS text
  AS '$libdir/pgcrypto',
  'pg_gen_salt' LANGUAGE 'C';

CREATE FUNCTION gen_salt(text, int4) RETURNS text
  AS '$libdir/pgcrypto',
  'pg_gen_salt_rounds' LANGUAGE 'C';

CREATE FUNCTION encrypt(bytea, bytea, text) RETURNS bytea
  AS '$libdir/pgcrypto',
  'pg_encrypt' LANGUAGE 'C';

CREATE FUNCTION decrypt(bytea, bytea, text) RETURNS bytea
  AS '$libdir/pgcrypto',
  'pg_decrypt' LANGUAGE 'C';

CREATE FUNCTION encrypt_iv(bytea, bytea, bytea, text) RETURNS bytea
  AS '$libdir/pgcrypto',
  'pg_encrypt_iv' LANGUAGE 'C';

CREATE FUNCTION decrypt_iv(bytea, bytea, bytea, text) RETURNS bytea
  AS '$libdir/pgcrypto',
  'pg_decrypt_iv' LANGUAGE 'C';

CREATE FUNCTION cipher_exists(text) RETURNS bool
  AS '$libdir/pgcrypto',
  'pg_cipher_exists' LANGUAGE 'C';



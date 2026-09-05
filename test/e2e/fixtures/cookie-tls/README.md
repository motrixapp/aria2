# Local TLS fixture

This self-signed certificate and **public test-only private key** are used only
by the loopback HTTPS server in `task-cookies.e2e.test.mjs`. They contain no
production credentials and must never be used for a deployed service. The test
passes this certificate explicitly as its CA; system trust is not modified.

该自签名证书及**公开的测试专用私钥**仅用于 cookie 验收测试的本机 HTTPS 服务，
不包含生产凭据，不能用于实际部署。测试显式指定此证书为 CA，不修改系统信任设置。

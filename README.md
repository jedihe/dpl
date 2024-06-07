# Drupalite (dpl)

Drupalite (dpl) is a super-minimalistic tool for deploying and troubleshooting Drupal sites.

## Installation

Add repository to composer.json:

```
composer config repositories.jedihe/dpl '{"type": "package", "package": {"name": "jedihe/dpl", "version": "dev-main", "type": "package", "bin": ["dpl"], "source": {"url": "https://github.com/jedihe/dpl.git", "type": "git", "reference": "main"}}}'
```

Require jedihe/dpl package:

```
composer require jedihe/dpl:dev-main
```

Quickly test it's running:

```
vendor/bin/dpl st
```

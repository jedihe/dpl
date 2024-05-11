<?php

use Drupal\Core\Utility\Error;

function dpl_updb_prepare($root_path) {
  global $dpl_logger;

  require_once $root_path . '/core/includes/install.inc';
  \drupal_load_updates();
  require_once $root_path . '/core/includes/update.inc';
  $pending_updates = \update_get_update_list();

  // Array structure: module_name => update_warnings.
  $updates_warnings = [];
  $start_updates = [];
  foreach ($pending_updates as $module => $updates) {
    if ($warning = $updates['warning'] ?? NULL) $updates_warnings[$module] = $warning;
    if ($start = $updates['start'] ?? NULL) $start_updates[$module] = $start;
  }

  /** @var \Drupal\Core\Update\UpdateRegistry $post_update_registry */
  $pending_post_updates = \Drupal::service('update.post_update_registry')->getPendingUpdateInformation();

  if (empty($pending_updates) && empty($pending_post_updates)) {
    $dpl_logger->notice('No pending updates.');
  }
  else {
    foreach ($updates_warnings as $module => $warning) {
      $dpl_logger->warning("$module: $warning"); // TODO: port drush's dt(), use placeholders for these messages.
    }
    echo json_encode(['updates' => $pending_updates, 'post-updates' => $pending_post_updates], JSON_PRETTY_PRINT);

    $runnable_sorted_updates = \update_resolve_dependencies($start_updates);

    $operations = [];
    foreach ($runnable_sorted_updates as $update) {
      if (empty($update['allowed'])) break;

      if (isset($start_updates[$update['module']])) {
        // TODO: setInstalledVersion in update.update_hook_registry introduced in Drupal 9.3.0
        if (!function_exists('drupal_set_installed_schema_version')) {
          \Drupal::service("update.update_hook_registry")->setInstalledVersion($update['module'], $update['number'] - 1);
        } else {
          drupal_set_installed_schema_version($update['module'], $update['number'] - 1);
        }
        unset($start_updates[$update['module']]);
      }
      $update_deps = array_keys($update['reverse_paths'] ?? []);
      $operations[] = ['dpl_updb_do_one', [$update['module'], $update['number'], $update_deps]];
    }

    $post_updates = \Drupal::service('update.post_update_registry')->getPendingUpdateFunctions();
    if ($post_updates && $operations) {
      // Only needed if we performed updates earlier.
      $operations[] = ['dpl_updb_cache_rebuild', []];
    }
    foreach ($post_updates as $function) {
      $operations[] = ['dpl_updb_do_one_post_update', [$function]];
    }

    $original_maint_mode = \Drupal::service('state')->get('system.maintenance_mode');
    if (!$original_maint_mode) {
      \Drupal::service('state')->set('system.maintenance_mode', true);
      $operations[] = ['dpl_updb_restore_maint_mode', [false]];
    }

    $batch = [
      'operations' => $operations,
      'title' => 'Updating',
      'init_message' => 'Starting updates',
      'error_message' => 'An unrecoverable error has occurred. You can find the error message below. It is advised to copy it to the clipboard for reference.',
      'file' => 'core/includes/update.inc',
    ];

    //print_r($batch);

    batch_set($batch);
    require_once __DIR__ . '/batch.inc';
    $result = drush_backend_batch_process('updb-batch');

    $success = false;
    if (!is_array($result)) {
      $type = gettype($result);
      $dpl_logger->error("Batch process did not return a result array. Returned: $type");
    }
    elseif (!empty($result[0]['#abort'])) {
      // Whenever an error occurs the batch process does not continue, so
      // this array should only contain a single item, but we still output
      // all available data for completeness.
      $abort_msg = implode(', ', $result[0]['#abort']);
      $dpl_logger->error("Update aborted by: $abort_msg");
    } else {
      $success = true;
    }
  }
}

function dpl_updb_do_one(string $module, int $number, array $dependency_map, array $context) {
  global $dpl_logger;

  $function = $module . '_update_' . $number;
  // echo print_r("About to run:" . $function);

  // Disable config entity overrides.
  if (!defined('MAINTENANCE_MODE')) {
      define('MAINTENANCE_MODE', 'update');
  }

  // If this update was aborted in a previous step, or has a dependency that
  // was aborted in a previous step, go no further.
  if (!empty($context['results']['#abort']) && array_intersect($context['results']['#abort'], array_merge($dependency_map, [$function]))) {
      return;
  }

  $context['log'] = false;

  \Drupal::moduleHandler()->loadInclude($module, 'install');

  $ret = [];
  if (function_exists($function)) {
      try {
          if ($context['log']) {
              Database::startLog($function);
          }

          if (empty($context['results'][$module][$number]['type'])) {
              $dpl_logger->notice("Update started: $function");
          }

          $ret['results']['query'] = $function($context['sandbox']);
          $ret['results']['success'] = true;
          $ret['type'] = 'update';
      } catch (\Throwable $e) {
          // PHP 7 introduces Throwable, which covers both Error and Exception throwables.
          $ret['#abort'] = ['success' => false, 'query' => $e->getMessage()];
          $dpl_logger->error($e->getMessage());
      }

      if ($context['log']) {
          $ret['queries'] = Database::getLog($function);
      }
  } else {
      $ret['#abort'] = ['success' => false];
      $dpl_logger->warning("Update function $function not found in file $module.install");
  }

  if (isset($context['sandbox']['#finished'])) {
      $context['finished'] = $context['sandbox']['#finished'];
      unset($context['sandbox']['#finished']);
  }

  if (!isset($context['results'][$module])) {
      $context['results'][$module] = [];
  }
  if (!isset($context['results'][$module][$number])) {
      $context['results'][$module][$number] = [];
  }
  $context['results'][$module][$number] = array_merge($context['results'][$module][$number], $ret);

  // Log the message that was returned.
  if (!empty($ret['results']['query'])) {
      $dpl_logger->notice(strip_tags((string) $ret['results']['query']));
  }

  if (!empty($ret['#abort'])) {
      // Record this function in the list of updates that were aborted.
      $context['results']['#abort'][] = $function;
      $dpl_logger->error("Update failed: $function");
  }

  // Record the schema update if it was completed successfully.
  if ($context['finished'] >= 1 && empty($ret['#abort'])) {
      \Drupal::service("update.update_hook_registry")->setInstalledVersion($module, $number);
      $context['message'] = "Update completed: $function";
  }
}
function dpl_updb_cache_rebuild() {
  drupal_flush_all_caches();
  \Drupal::service('kernel')->rebuildContainer();
  // Load the module data which has been removed when the container was
  // rebuilt.
  $module_handler = \Drupal::moduleHandler();
  $module_handler->loadAll();
  $module_handler->invokeAll('rebuild');
}
function dpl_updb_do_one_post_update(string $function, array $context) {
  global $dpl_logger;

  $ret = [];

  // Disable config entity overrides.
  if (!defined('MAINTENANCE_MODE')) {
    define('MAINTENANCE_MODE', 'update');
  }

  // If this update was aborted in a previous step, or has a dependency that was
  // aborted in a previous step, go no further.
  if (!empty($context['results']['#abort'])) {
    return;
  }

  list($extension, $name) = explode('_post_update_', $function, 2);
  $update_registry = \Drupal::service('update.post_update_registry');
  // https://www.drupal.org/project/drupal/issues/3259188 Support theme's
  // having post update functions when it is supported in Drupal core.
  if (method_exists($update_registry, 'getUpdateFunctions')) {
    \Drupal::service('update.post_update_registry')->getUpdateFunctions($extension);
  } else {
    \Drupal::service('update.post_update_registry')->getModuleUpdateFunctions($extension);
  }

  if (function_exists($function)) {
    if (empty($context['results'][$extension][$name]['type'])) {
      $dpl_logger->notice("Update started: $function");
    }
    try {
      $ret['results']['query'] = $function($context['sandbox']);
      $ret['results']['success'] = true;
      $ret['type'] = 'post_update';

      if (!isset($context['sandbox']['#finished']) || (isset($context['sandbox']['#finished']) && $context['sandbox']['#finished'] >= 1)) {
        \Drupal::service('update.post_update_registry')->registerInvokedUpdates([$function]);
      }
    } catch (\Exception $e) {
      // @TODO We may want to do different error handling for different exception
      // types, but for now we'll just log the exception and return the message
      // for printing.
      // @see https://www.drupal.org/node/2564311
      $dpl_logger->error($e->getMessage());

      $variables = Error::decodeException($e);
      unset($variables['backtrace']);
      $ret['#abort'] = [
        'success' => false,
        'query' => t('%type: @message in %function (line %line of %file).', $variables),
      ];
    }
  } else {
    $ret['#abort'] = ['success' => false];
    $dpl_logger->warning(t('Post update function @function not found in file @filename', [
      '@function' => $function,
      '@filename' => "$filename.php",
    ]));
  }

  if (isset($context['sandbox']['#finished'])) {
    $context['finished'] = $context['sandbox']['#finished'];
    unset($context['sandbox']['#finished']);
  }
  if (!isset($context['results'][$extension][$name])) {
    $context['results'][$extension][$name] = [];
  }
  $context['results'][$extension][$name] = array_merge($context['results'][$extension][$name], $ret);

  // Log the message that was returned.
  if (!empty($ret['results']['query'])) {
    $dpl_logger->notice(strip_tags((string) $ret['results']['query']));
  }

  if (!empty($ret['#abort'])) {
    // Record this function in the list of updates that were aborted.
    $context['results']['#abort'][] = $function;
    $dpl_logger->error("Update failed: $function");
  } elseif ($context['finished'] == 1 && empty($ret['#abort'])) {
    $context['message'] = "Update completed: $function";
  }
}

function dpl_updb_restore_maint_mode($status) {
  \Drupal::service('state')->set('system.maintenance_mode', $status);
}

<?php

/**
 * Copyright (C) 2013-2015 OpenMediaVault Plugin Developers
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

require_once "openmediavault/config.inc";
require_once "openmediavault/error.inc";
require_once "openmediavault/notify.inc";
require_once "openmediavault/object.inc";
require_once "openmediavault/rpcservice.inc";

class OMVRpcServiceMylar extends OMVRpcServiceAbstract
{
    private $mylarSettingsDir = "/var/opt/mylar/";
    private $mylarSettingsFile = "/var/opt/mylar/config.ini";

    private function getPluginName()
    {
        return strtolower($this->getName());
    }

    private function getEventMessagePath()
    {
        return sprintf("org.openmediavault.service.%s", $this->getPluginName());
    }

    private function getXpath()
    {
        return sprintf("//services/%s", $this->getPluginName());
    }

    private function getForksXpath()
    {
        return sprintf("%s/forks", $this->getXpath());
    }

    private function getForkXpath()
    {
        return sprintf("%s/fork", $this->getForksXpath());
    }

    public function getName()
    {
        return "Mylar";
    }

    public function initialize()
    {
        $this->registerMethod("getSettings");
        $this->registerMethod("setSettings");
        $this->registerMethod("enumerateRepos");

        $this->registerMethod("getForks");
        $this->registerMethod("getFork");
        $this->registerMethod("setFork");
        $this->registerMethod("deleteFork");

        $this->registerMethod("doBackup");
        $this->registerMethod("doRestore");
    }

    public function getSettings($params, $context)
    {
        global $xmlConfig;

        $this->validateMethodContext($context, array(
            "role" => OMV_ROLE_ADMINISTRATOR
        ));

        $object = $xmlConfig->get($this->getXpath());

        if (is_null($object)) {
            throw new OMVException(
                OMVErrorMsg::E_CONFIG_GET_OBJECT_FAILED,
                $this->getXpath()
            );
        }

        $port = 8090;

        // Get current port as set in Mylars config file.
        if (file_exists($this->mylarSettingsFile)) {
            $config = parse_ini_file(
                $this->mylarSettingsFile,
                true,
                INI_SCANNER_RAW
            );

            if (!empty($config["General"]["port"])) {
                $port = $config["General"]["port"];
            }

            if (!empty($config["General"]["web_port"])) {
                $port = $config["General"]["web_port"];
            }
        }

        $object["enable"]         = boolval($object["enable"]);
        $object["showtab"]        = boolval($object["showtab"]);
        $object["ssl"]            = boolval($object["ssl"]);
        $object["ppass"]          = boolval($object["ppass"]);
        $object["port"]           = intval($port);

        // Remove useless fields from the result object.
        unset($object["forks"]);

        return $object;
    }

    public function setSettings($params, $context)
    {
        global $xmlConfig;

        $this->validateMethodContext($context, array(
            "role" => OMV_ROLE_ADMINISTRATOR
        ));

        $this->validateMethodParams(
            $params,
            '{
                "type": "object",
                "properties": {
                    "enable"        : { "type" : "boolean" },
                    "showtab"       : { "type" : "boolean" },
                    "ppass"         : { "type" : "boolean" },
                    "ssl"           : { "type" : "boolean" },
                    "branch"        : { "type" : "string" },
                    "repo"          : { "type" : "string" },
                    "port"          : { "type" : "integer" }
                }
            }'
        );

        // Get existing configuration object
        $oldObject = $xmlConfig->get($this->getXpath());

        if (is_null($oldObject)) {
            throw new OMVException(
                OMVErrorMsg::E_CONFIG_GET_OBJECT_FAILED,
                $this->getXpath()
            );
        }

        // Run checks to verify sellected settings
        if ($params["ssl"] == "1") {
            if ($params["ppass"] == "1") {
                throw new OMVException(
                    OMVErrorMsg::E_MISC_FAILURE,
                    "You can not enable both SSL and Proxy Pass."
                );
            }
            $xSsl = $xmlConfig->get("//webadmin/enablessl");
            if ($xSsl == "1") {
                throw new OMVException(
                    OMVErrorMsg::E_MISC_FAILURE,
                    "You have SSL enabled for the main UI, Do NOT enable it here as well."
                );
            }
            $ySsl = $xmlConfig->get("//system/certificates/sslcertificate/uuid");
            if ($ySsl == "") {
                throw new OMVException(
                    OMVErrorMsg::E_MISC_FAILURE,
                    "You have not generated any SSL certificates."
                );
            }
        }

        if ($params["showtab"] == 1 && $params["ppass"] != 1) {
            if (! file_exists('/var/www/openmediavault/js/omv/module/admin/service/media/Media.js')) {
                throw new OMVException(
                    OMVErrorMsg::E_MISC_FAILURE,
                    "Install links and add an entry or enable Proxy Pass to use the web tab."
                );
            }
        }

        $object = array(
            "enable"        => array_boolval($params, "enable"),
            "showtab"       => array_boolval($params, "showtab"),
            "ppass"         => array_boolval($params, "ppass"),
            "ssl"           => array_boolval($params, "ssl"),
            "branch"        => $params["branch"],
            "repo"          => $params["repo"],
            "port"          => $params["port"],
            "forks"         => $oldObject['forks'],
        );

        if ($xmlConfig->replace($this->getXpath(), $object) === false) {
            throw new OMVException(
                OMVErrorMsg::E_CONFIG_SET_OBJECT_FAILED,
                $this->getXpath()
            );
        }

        $dispatcher = &OMVNotifyDispatcher::getInstance();
        $dispatcher->notify(
            OMV_NOTIFY_MODIFY,
            $this->getEventMessagePath(),
            $object
        );

        return $object;
    }

    public function enumerateRepos($params, $context)
    {
        global $xmlConfig;

        $this->validateMethodContext($context, array(
            "role" => OMV_ROLE_ADMINISTRATOR
        ));

        // Get configuration data.
        $userRepos = $xmlConfig->getList($this->getForkXpath());

        if (is_null($userRepos)) {
            throw new OMVException(
                OMVErrorMsg::E_CONFIG_GET_OBJECT_FAILED,
                $this->getForkXpath()
            );
        }

        // Add default repos and their branches. Do it here so users can't
        // delete them.
        $defaultRepos = array(
            array(
                "uuid" => "11111111-2222-3333-4444-555555555555",
                "name" => "Main Branch1",
                "fork" => "https://github.com/evilhero/mylar.git",
                "branches" => array(
                    "development",
                    "master"
                )
            )
        );

        $repos = array_merge($defaultRepos, $userRepos);

        return $repos;
    }

    public function getForks($params, $context)
    {
        global $xmlConfig;

        // Validate the RPC caller context.
        $this->validateMethodContext($context, array(
            "role" => OMV_ROLE_ADMINISTRATOR
        ));

        // Validate the parameters of the RPC service method.
        $this->validateMethodParams(
            $params,
            '{
                "type"       : "object",
                "properties" : {
                    "start"     : { "type" : "integer" },
                    "limit"     : { ' . $GLOBALS["OMV_JSONSCHEMA_COUNTFIELD"] . ' },
                    "sortfield" : { ' . $GLOBALS["OMV_JSONSCHEMA_SORTFIELD"] . ' },
                    "sortdir"   : { ' . $GLOBALS["OMV_JSONSCHEMA_SORTDIR"] . ' }
                }
            }'
        );

        // Get configuration data.
        $objects = $xmlConfig->getList($this->getForkXpath());

        if (is_null($objects)) {
            throw new OMVException(OMVErrorMsg::E_CONFIG_GET_OBJECT_FAILED, $this->getForkXpath());
        }

        // Filter the result.
        return $this->applyFilter(
            $objects,
            $params["start"],
            $params["limit"],
            $params["sortfield"],
            $params["sortdir"]
        );
    }

    public function getFork($params, $context)
    {
        global $xmlConfig;

        // Validate the RPC caller context.
        $this->validateMethodContext($context, array(
            "role" => OMV_ROLE_ADMINISTRATOR
        ));

        // Validate the parameters of the RPC service method.
        $this->validateMethodParams(
            $params,
            '{
                "type"       : "object",
                "properties" : {
                    "uuid" : { ' . $GLOBALS["OMV_JSONSCHEMA_UUID"] . ' }
                }
            }'
        );

        // Get the configuration object.
        $xpath = sprintf("%s[uuid='%s']", $this->getForkXpath(), $params["uuid"]);
        $object = $xmlConfig->get($xpath);

        if (is_null($object)) {
            throw new OMVException(
                OMVErrorMsg::E_CONFIG_GET_OBJECT_FAILED,
                $xpath
            );
        }

        return $object;
    }

    public function setFork($params, $context)
    {
        global $xmlConfig;

        // Validate the RPC caller context.
        $this->validateMethodContext($context, array(
            "role" => OMV_ROLE_ADMINISTRATOR
        ));

        // Validate the parameters of the RPC service method.
        $this->validateMethodParams(
            $params,
            '{
                "type"       : "object",
                "properties" : {
                    "uuid" : { ' . $GLOBALS["OMV_JSONSCHEMA_UUID_UNDEFINED"] . ' },
                    "name" : { "type" : "string" },
                    "fork" : { "type" : "string" }
                }
            }'
        );

        // Check if valid fork. A valid fork should start with
        // either http(s):// or git://
        if (substr($params["fork"], 0, 4) !== "http" &&
            substr($params["fork"], 0, 3) !== "git"
        ) {
            throw new OMVException(
                OMVErrorMsg::E_EXEC_MISC,
                "Not valid git repo"
            );
        }

        // Query repo for a list of branches.
        $cmd = "git ls-remote -h "
             . $params["fork"]
             . " | sed 's?.*refs/heads/??'";

        $this->exec($cmd, $output);

        if (empty($output)) {
            throw new OMVException(
                OMVErrorMsg::E_EXEC_MISC,
                "No branches found, check url."
            );
        }

        if (substr($output, 0, 5) === "fatal") {
            throw new OMVException(
                OMVErrorMsg::E_EXEC_MISC,
                "No branches found, is it a valid git repo?"
            );
        }

        // Prepare configuration data and add all branches found.
        $object = array(
            "uuid"        => ($params["uuid"] == $GLOBALS["OMV_UUID_UNDEFINED"])
                ? OMVUtil::uuid() : $params["uuid"],
            "name"        => $params["name"],
            "fork"        => $params["fork"],
            "branches"    => $output,
        );

        // Set the configuration data.
        $success = false;

        if ($params["uuid"] == $GLOBALS["OMV_UUID_UNDEFINED"]) {
            // Append object to configuration.
            $success = $xmlConfig->set(
                $this->getForksXpath(),
                array("fork" => $object)
            );
        } else {
            // Update existing configuration object.
            $xpath = sprintf(
                "%s[uuid='%s']",
                $this->getForkXpath(),
                $params["uuid"]
            );
        }

        if ($success === false) {
            throw new OMVException(OMVErrorMsg::E_CONFIG_SET_OBJECT_FAILED);
        }

        // Return the configuration object.
        return $object;
    }

    public function deleteFork($params, $context)
    {
        global $xmlConfig;

        // Validate the RPC caller context.
        $this->validateMethodContext($context, array(
            "role" => OMV_ROLE_ADMINISTRATOR
        ));

        // Validate the parameters of the RPC service method.
        $this->validateMethodParams(
            $params,
            '{
                "type"       : "object",
                "properties" : {
                    "uuid" : { ' . $GLOBALS["OMV_JSONSCHEMA_UUID"] . ' }
                }
            }'
        );

        // Delete the configuration object.
        $xpath = sprintf("%s[uuid='%s']", $this->getForkXpath(), $params["uuid"]);

        return $this->deleteConfigObjectByPath($xpath);
    }

    public function doBackup($params, $context)
    {
        // Validate the RPC caller context.
        $this->validateMethodContext($context, array(
            "role" => OMV_ROLE_ADMINISTRATOR
        ));

        // Validate the parameters of the RPC service method.
        $this->validateMethodParams(
            $params,
            '{
                "type": "object",
                "properties": {
                    "sharedfolderref": { ' . $GLOBALS["OMV_JSONSCHEMA_UUID"] . ' }
                }
            }'
        );

        // Mylar needs to have run once to generate its settings. Check if this is true
        if (!file_exists($this->mylarSettingsFile)) {
            throw new OMVException(
                OMVErrorMsg::E_EXEC_MISC,
                "No settings folder found!"
            );
        }

        // Validate the RPC caller context.
        $this->validateMethodContext($context, array(
            "role" => OMV_ROLE_ADMINISTRATOR
        ));

        // Create a background process.
        $bgStatusFilename = $this->createBgProcStatus();
        $pid = $this->fork();

        // Parent process.
        if ($pid > 0) {
            $this->initializeBgProcStatus($bgStatusFilename, $pid);
            return $bgStatusFilename;
        }

        // Get the Mylar module
        $moduleMgr = &OMVModuleMgr::getInstance();
        $mylarModule = $moduleMgr->getModule($this->getName());

        // Child process.
        try {
            $mylarModule->stopService();
            $bgOutputFilename = $this->createBgProcOutput();
            $this->updateBgProcStatus(
                $bgStatusFilename,
                "outputfilename",
                $bgOutputFilename
            );

            $backupPath = sprintf(
                "%s%smylar.tar.gz",
                $this->getSharedFolderPathByUuid($params["sharedfolderref"]),
                DIRECTORY_SEPARATOR
            );

            $cmd = sprintf(
                    "tar -zcvPf %s %s 2>&1",
                    $backupPath,
                    $this->mylarSettingsDir
                );

            $this->exec($cmd, $output, $bgOutputFilename);
            $this->finalizeBgProcStatus($bgStatusFilename, $output);
            $mylarModule->startService();
            exit(0);
        } catch (Exception $e) {
            $mylarModule->startService();
            $this->finalizeBgProcStatus($bgStatusFilename, "", $e);
            exit(1);
        }
    }

    public function doRestore($params, $context)
    {
        // Validate the RPC caller context.
        $this->validateMethodContext($context, array(
            "role" => OMV_ROLE_ADMINISTRATOR
        ));

        // Validate the parameters of the RPC service method.
        $this->validateMethodParams(
            $params,
            '{
                "type": "object",
                "properties": {
                    "sharedfolderref": { ' . $GLOBALS["OMV_JSONSCHEMA_UUID"] . ' }
                }
            }'
        );

        // Validate the RPC caller context.
        $this->validateMethodContext($context, array(
            "role" => OMV_ROLE_ADMINISTRATOR
        ));

        $backupPath = sprintf(
            "%s%smylar.tar.gz",
            $this->getSharedFolderPathByUuid($params["sharedfolderref"]),
            DIRECTORY_SEPARATOR
        );

        if (is_null($backupPath)) {
            throw new OMVException(
                OMVErrorMsg::E_CONFIG_GET_OBJECT_FAILED,
                $xpath
            );
        }

        // Check if backup is present.
        if (file_exists($backupPath)) {
            // Create a background process.
            $bgStatusFilename = $this->createBgProcStatus();
            $pid = $this->fork();

            // Parent process.
            if ($pid > 0) {
                $this->initializeBgProcStatus($bgStatusFilename, $pid);
                return $bgStatusFilename;
            }

            // Delete current settings folder
            rmdir($this->mylarSettingsDir);

            // Get the Mylar module
            $moduleMgr = &OMVModuleMgr::getInstance();
            $mylarModule = $moduleMgr->getModule($this->getName());

            // Child process.
            try {
                $mylarModule->stopService();
                $bgOutputFilename = $this->createBgProcOutput();
                $this->updateBgProcStatus(
                    $bgStatusFilename,
                    "outputfilename",
                    $bgOutputFilename
                );

                $cmd = sprintf("tar -zxvf %s -C / 2>&1", $backupPath);

                $this->exec($cmd, $output, $bgOutputFilename);
                $this->finalizeBgProcStatus($bgStatusFilename, $output);
                $mylarModule->startService();
                exit(0);
            } catch (Exception $e) {
                $this->finalizeBgProcStatus($bgStatusFilename, "", $e);
                $this->exec("omv-mkconf mylar");
                exit(1);
            }
        } else {
            throw new OMVException(OMVErrorMsg::E_EXEC_MISC, "No backup found!");
        }
    }

    private function getSharedFolderPathByUuid($uuid)
    {
        global $xmlConfig;

        $xpath = sprintf("//system/shares/sharedfolder[uuid='%s']", $uuid);
        $sharedFolder = $xmlConfig->get($xpath);
        $sharedFolderRelDirPath = $sharedFolder["reldirpath"];

        $xpath = sprintf(
            "//system/fstab/mntent[uuid='%s']",
            $sharedFolder["mntentref"]
        );
        $mountPoint = $xmlConfig->get($xpath);
        $mountPointDir = $mountPoint["dir"];

        return $mountPointDir . DIRECTORY_SEPARATOR . $sharedFolderRelDirPath;
    }
}

// Register the RPC service.
$rpcServiceMgr = &OMVRpcServiceMgr::getInstance();
$rpcServiceMgr->registerService(new OMVRpcServiceMylar());

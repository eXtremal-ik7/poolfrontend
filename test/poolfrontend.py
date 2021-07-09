import json
import requests
import sys

class Poolfrontend:
    URL=""

    def __init__(self, url):
        self.URL = url

    def __call__(self, functionName, data, requiredStatus=None, debug=None):
        print(requiredStatus)
        result = requests.post(self.URL + "/" + functionName, json=data).json()
        if debug is True:
            print("curl -X POST -d '{}' {}/{}\n{}\n".format(json.dumps(data), self.URL, functionName,
                                                            json.dumps(result, indent=2)))
        if requiredStatus is not None:
            if result["status"] != requiredStatus:
                raise Exception("{} failed".format(functionName))
        return result

    def userCreate(self, name, password, email, sessionId=None, isActive=None, isReadOnly=None, feePlan=None, requiredStatus=None, debug=None):
        data = {"login": name, "password": password, "email": email}
        if sessionId is not None:
            data.update({"id": sessionId})
        if isActive is not None:
            data.update({"isActive": isActive})
        if isReadOnly is not None:
            data.update({"isReadOnly": isReadOnly})
        if feePlan is not None:
            data.update({"feePlanId": feePlan})
        return self.__call__("userCreate", data, requiredStatus, debug)

    def userAction(self, actionId, sessionId=None, targetLogin=None, newPassword=None, totp=None, requiredStatus=None, debug=None):
        data = {"actionId": actionId}
        if sessionId is not None:
            data.update({"sessionId": sessionId})
        if targetLogin is not None:
            data.update({"targetLogin": targetLogin})
        if newPassword is not None:
            data.update({"newPassword": newPassword})
        if totp is not None:
            data.update({"totp": totp})
        return self.__call__("userAction", data, requiredStatus, debug)

    def userLogin(self, name, password, totp=None, requiredStatus=None, debug=None):
        data = {"login": name, "password": password}
        if totp is not None:
            data.update({"totp": totp})
        return self.__call__("userLogin", data, requiredStatus, debug)

    def userLogout(self, sessionId, requiredStatus=None, debug=None):
        return self.__call__("userLogout", {"id": sessionId}, requiredStatus, debug)

    def userChangePasswordInitiate(self, login, requiredStatus=None, debug=None):
        return self.__call__("userChangePasswordInitiate", {"login": login}, requiredStatus, debug)

    def userActivate2faInitiate(self, sessionId, targetLogin=None, requiredStatus=None, debug=None):
        data = {"sessionId": sessionId}
        if targetLogin is not None:
            data.update({"targetLogin": targetLogin})
        return self.__call__("userActivate2faInitiate", data, requiredStatus, debug)

    def userDeactivate2faInitiate(self, sessionId, targetLogin=None, requiredStatus=None, debug=None):
        data = {"sessionId": sessionId}
        if targetLogin is not None:
            data.update({"targetLogin": targetLogin})
        return self.__call__("userDeactivate2faInitiate", data, requiredStatus, debug)

    def userUpdateSettings(self, sessionId, coin, address, payoutThreshold, autoPayoutEnabled, targetLogin=None, totp=None, requiredStatus=None, debug=None):
        data = {"id": sessionId, "coin": coin, "address": address, "payoutThreshold": payoutThreshold, "autoPayoutEnabled": autoPayoutEnabled}
        if targetLogin is not None:
            data.update({"targetLogin": targetLogin})
        if totp is not None:
            data.update({"totp": totp})
        return self.__call__("userUpdateSettings", data, requiredStatus, debug)
